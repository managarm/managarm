
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

#include <vector>
#include <memory>

#include <hel.h>
#include <hel-syscalls.h>
#include <helx.hpp>

#include <frigg/atomic.hpp>

#include <bragi/mbus.hpp>
#include <hw.pb.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#include <cairo.h>
#include <cairo-ft.h>

enum {
	kRegXres = 1,
	kRegYres = 2,
	kRegBpp = 3,
	kRegEnable = 4,
	kRegOffsetX = 8,
	kRegOffsetY = 9
};

enum {
	kBpp24 = 0x18,
	kBpp32 = 0x20,

	// enable register bits
	kEnabled = 0x01,
	kLinearFramebuffer = 0x40
};

enum {
	kSolarBase03 = 0x002B36,
	kSolarBase02 = 0x073642,
	kSolarBase01 = 0x586E75,
	kSolarBase00 = 0x657B83,
	kSolarBase0 = 0x839496,
	kSolarBase1 = 0x93A1A1,
	kSolarBase2 = 0xEEE8D5,
	kSolarBase3 = 0xFDF6E3,
	kSolarYellow = 0xB58900,
	kSolarOrange = 0xCB4B16,
	kSolarRed = 0xDC322F,
	kSolarMargenta = 0xD33682,
	kSolarViolet = 0x6C71C4,
	kSolarBlue = 0x268BD2,
	kSolarCyan = 0x2AA198,
	kSolarGreen = 0x859900
};

enum Layout {
	kNoLayout = 0,
	kHorizontalBlocks = 1,
	kVerticalBlocks = 2,
	kLayoutLine = 3
};

struct RgbColor {
	float r;
	float g;
	float b;
};

RgbColor rgbFromInt(uint32_t color) {
	RgbColor rgb;
	rgb.r = ((color >> 16) & 0xFF) / 255.0;
	rgb.g = ((color >> 8) & 0xFF) / 255.0;
	rgb.b = (color & 0xFF) / 255.0;
	return rgb;
}

struct Box {
	enum SizeType {
		kSizeFixed,
		kSizeFitToChildren,
		kSizeFillParent
	};

	Box();
	void appendChild(std::shared_ptr<Box> child);

	int x;
	int y;
	
	SizeType widthType;
	SizeType heightType;
	int fixedWidth;
	int fixedHeight;
	
	int actualWidth;
	int actualHeight;
	uint32_t backgroundColor;
	bool hasBorder;
	int borderWidth;
	uint32_t borderColor;
	
	int padding;
	int margin;

	Layout layout;
	
	bool hasText;
	std::string text;
	double fontSize;
	uint32_t fontColor;

	Box *parent;
	std::vector<std::shared_ptr<Box>> children;
};

Box::Box() : hasBorder(false), borderWidth(0), hasText(false) { }

void Box::appendChild(std::shared_ptr<Box> child) {
	child->parent = this;
	children.push_back(child);
}

FT_Library ftLibrary;
FT_Face ftFace;
cairo_font_face_t *crFont;
std::shared_ptr<Box> rootBox;

void drawBox(cairo_t *cr, Box *box) {	
	// border
	if(box->hasBorder) {
		auto rgb = rgbFromInt(box->borderColor);
		cairo_set_source_rgb(cr, rgb.r, rgb.g, rgb.b);	
		cairo_rectangle(cr, box->x - box->borderWidth, box->y - box->borderWidth,
				box->actualWidth + box->borderWidth * 2, box->actualHeight + box->borderWidth * 2);
		cairo_fill(cr);
	}

	// box
	auto rgb = rgbFromInt(box->backgroundColor);
	cairo_set_source_rgb(cr, rgb.r, rgb.g, rgb.b);
	cairo_rectangle(cr, box->x, box->y, box->actualWidth, box->actualHeight);
	cairo_fill(cr);

	if(box->hasText) {
		// FT_Set_Char_Size() with DPI = 0 is equivalent to FT_Set_Pixel_Sizes()
		// but allows fractional pixel values
		if(FT_Set_Char_Size(ftFace, box->fontSize * 64.0, 0, 0, 0) != 0) {
			printf("FT_Set_Char_Size() failed\n");
			abort();
		}
		
		int glyph_count = box->text.length();
		cairo_glyph_t *cairo_glyphs = new cairo_glyph_t[glyph_count];

		int x = box->x + box->padding;
		int base_y = box->y + box->padding + box->fontSize;
		for(int i = 0; i < glyph_count; i++) {
			FT_UInt glyph_index = FT_Get_Char_Index(ftFace, box->text[i]);
			if(glyph_index == 0) {
				printf("FT_Get_Char_Index() failed\n");
				abort();
			}

			if(FT_Load_Glyph(ftFace, glyph_index, 0)) {
				printf("FT_Load_Glyph() failed\n");
				abort();
			}

			FT_Glyph_Metrics &metrics = ftFace->glyph->metrics;

			cairo_glyphs[i].index = glyph_index;
			cairo_glyphs[i].x = x;
			cairo_glyphs[i].y = base_y;

			x += metrics.horiAdvance >> 6;
		}

		auto fontColor = rgbFromInt(box->fontColor);
		cairo_set_source_rgb(cr, fontColor.r, fontColor.g, fontColor.b);
		cairo_set_font_face(cr, crFont);
		cairo_set_font_size(cr, box->fontSize);
		cairo_show_glyphs(cr, cairo_glyphs, glyph_count);
		delete[] cairo_glyphs;
	}

	for(unsigned int i = 0; i < box->children.size(); i++) {
		drawBox(cr, box->children[i].get());
	}
}

void layoutChildren(Box *box) {
	// if the parent is kSizeFixed or kSizeFillParent its size is already computed at this point

	if(box->widthType == Box::kSizeFixed) {
		box->actualWidth = box->fixedWidth;
	}else if(box->widthType == Box::kSizeFillParent) {
		assert(box->parent->widthType == Box::kSizeFixed
				|| box->parent->widthType == Box::kSizeFillParent);
		box->actualWidth = box->parent->actualWidth - box->borderWidth * 2 - box->parent->padding * 2 - box->margin * 2;
	}

	if(box->heightType == Box::kSizeFixed) {
		box->actualHeight = box->fixedHeight;
	}else if(box->heightType == Box::kSizeFillParent) {
		assert(box->parent->heightType == Box::kSizeFixed
				|| box->parent->heightType == Box::kSizeFillParent);
		box->actualHeight = box->parent->actualHeight - box->borderWidth * 2 - box->parent->padding * 2 - box->margin * 2;
	}

	// for kSizeFixed and kSizeFillParent the size is computed at this point
	// kFitToChildren must be computed AFTER children are layouted

	if(box->layout == kNoLayout) {
		// do nothing
	}else if(box->layout == kLayoutLine) {
		// FT_Set_Char_Size() with DPI = 0 is equivalent to FT_Set_Pixel_Sizes()
		// but allows fractional pixel values
		if(FT_Set_Char_Size(ftFace, box->fontSize * 64.0, 0, 0, 0) != 0) {
			printf("FT_Set_Char_Size() failed\n");
			abort();
		}

		box->actualWidth = 0;
		for(unsigned int i = 0; i < box->text.length(); i++) {
			FT_UInt glyph_index = FT_Get_Char_Index(ftFace, box->text[i]);
			if(glyph_index == 0) {
				printf("FT_Get_Char_Index() failed\n");
				abort();
			}

			if(FT_Load_Glyph(ftFace, glyph_index, 0)) {
				printf("FT_Load_Glyph() failed\n");
				abort();
			}

			FT_Glyph_Metrics &g_metrics = ftFace->glyph->metrics;
			box->actualWidth += g_metrics.horiAdvance >> 6;
		}

		FT_Size_Metrics &s_metrics = ftFace->size->metrics;
		box->actualHeight = s_metrics.height >> 6;
	}else if(box->layout == kVerticalBlocks) {
		int accumulated_y = 0;
		for(unsigned int i = 0; i < box->children.size(); i++) {
			auto child = box->children[i].get();
			child->x = box->x + box->padding + child->borderWidth + child->margin;
			child->y = box->y + box->padding + accumulated_y + child->borderWidth + child->margin;
			
			assert(child->heightType == Box::kSizeFixed);
			layoutChildren(child);
			accumulated_y += child->actualHeight + child->borderWidth * 2 + child->margin * 2;
		}
	}else if(box->layout == kHorizontalBlocks) {
		int accumulated_x = 0;
		for(unsigned int i = 0; i < box->children.size(); i++) {
			auto child = box->children[i].get();
			child->y = box->y + box->padding + child->borderWidth + child->margin;
			child->x = box->x + box->padding + accumulated_x + child->borderWidth + child->margin;

			assert(child->widthType == Box::kSizeFixed);
			layoutChildren(child);
			accumulated_x += child->actualWidth + child->borderWidth * 2 + child->margin * 2;
		}
	}else{
		assert(!"Illegal Layout!");
	}

	if(box->widthType == Box::kSizeFitToChildren) {
		int child_width = 0;
		for(unsigned int i = 0; i < box->children.size(); i++) {
			auto child = box->children[i].get();
			child_width += child->actualWidth + child->borderWidth * 2 + child->margin * 2;
		}
		box->actualWidth = child_width + box->padding * 2;
	}
	
	if(box->heightType == Box::kSizeFitToChildren) {
		int child_height = 0;
		for(unsigned int i = 0; i < box->children.size(); i++) {
			auto child = box->children[i].get();
			child_height += child->actualHeight + child->borderWidth * 2 + child->margin * 2;
		}
		box->actualHeight = child_height + box->padding * 2;
	}
}

struct TerminalWidget {
	TerminalWidget(int width, int height, double font);

	int width;
	int height;
	double fontSize;
	std::shared_ptr<Box> mainBox;

	std::shared_ptr<Box> getBox();
	void setChar(int x, int y, char c);
};
TerminalWidget::TerminalWidget(int width, int height, double font) {
	this->width = width;
	this->height = height;
	this->fontSize = font;

	mainBox = std::make_shared<Box>();
	mainBox->layout = kVerticalBlocks;

	mainBox->widthType = Box::kSizeFillParent;
	mainBox->heightType = Box::kSizeFixed;

	mainBox->fixedWidth = font * width;
	mainBox->fixedHeight = font * height;	

	for(int i = 0; i < height; i++) {
		auto box = std::make_shared<Box>();
		box->layout = kHorizontalBlocks;

		box->widthType = Box::kSizeFillParent;
		box->fixedHeight = font;
		box->heightType = Box::kSizeFixed;
		box->backgroundColor = 0xFFFFFF;

		mainBox->appendChild(box);
		for(int j = 0; j < width; j++) {
			auto child_box = std::make_shared<Box>();
			child_box->layout = kLayoutLine;

			child_box->fixedWidth = font;
			child_box->widthType = Box::kSizeFixed;
			child_box->heightType = Box::kSizeFillParent;
			child_box->backgroundColor = 0xFFFFFF;

			child_box->hasText = true;
			child_box->text = " ";
			child_box->fontSize = font;
			child_box->fontColor = 0x000000;

			mainBox->children[i]->appendChild(child_box);
		}
	}
}

std::shared_ptr<Box> TerminalWidget::getBox() {
	return mainBox;
}

void TerminalWidget::setChar(int x, int y, char c) {
	//assert(y > (int)mainBox->children.size());
	//assert(x > (int)mainBox->children[y]->children.size());

	auto box = mainBox->children[y]->children[x];
	box->text = std::string(1, c);
}

void writeReg(uint16_t index, uint16_t value) {
	asm volatile ( "outw %0, %1" : : "a" (index), "d" (uint16_t(0x1CE)) );

	asm volatile ( "outw %0, %1" : : "a" (value), "d" (uint16_t(0x1CF)) );
}
uint16_t readReg(uint16_t index, uint16_t value) {
	asm volatile ( "outw %0, %1" : : "a" (index), "d" (uint16_t(0x1CE)) );

	uint16_t result;
	asm volatile ( "inw %1, %0" : "=a" (result) : "d" (uint16_t(0x1CF)) );
	return result;
}

int width = 1024, height = 786;
uint8_t *pixels;

void setPixel(int x, int y, uint8_t r, uint8_t g, uint8_t b) {
	pixels[(y * width + x) * 4] = b;
	pixels[(y * width + x) * 4 + 1] = g;
	pixels[(y * width + x) * 4 + 2] = r;
}

helx::EventHub eventHub = helx::EventHub::create();
bragi_mbus::Connection mbusConnection(eventHub);

// --------------------------------------------------------
// InitClosure
// --------------------------------------------------------

struct InitClosure {
	void operator() ();

private:
	void connected();
	void enumeratedBochs(std::vector<bragi_mbus::ObjectId> objects);
	void queriedBochs(HelHandle handle);
};

void InitClosure::operator() () {
	mbusConnection.connect(CALLBACK_MEMBER(this, &InitClosure::connected));
}

void InitClosure::connected() {
	mbusConnection.enumerate("pci-vendor:0x1234",
			CALLBACK_MEMBER(this, &InitClosure::enumeratedBochs));
}

void InitClosure::enumeratedBochs(std::vector<bragi_mbus::ObjectId> objects) {
	assert(objects.size() == 1);
	mbusConnection.queryIf(objects[0],
			CALLBACK_MEMBER(this, &InitClosure::queriedBochs));
}

struct Display {
	struct Buffer {
		cairo_surface_t *crSurface;
		cairo_t *crContext;
		int offsetY;

		Buffer()
		: crSurface(nullptr), crContext(nullptr), offsetY(0) { }
	};

	Display()
	: pending(0) { }

	cairo_t *getContext() {
		return buffers[pending].crContext;
	}

	void flip() {
		cairo_surface_flush(buffers[pending].crSurface);
		writeReg(kRegOffsetY, buffers[pending].offsetY);
		pending = (pending + 1) % 2;
	}

	Buffer buffers[2];
	int pending;
};

Display display;	
int fpsCurrent, fpsCounter;
uint64_t fpsLastTick;

void drawFrame(Display *display, int number) {
	cairo_t *cr_context = display->getContext();
	
	// clear the screen
	auto bgcolor = rgbFromInt(kSolarBase03);
	cairo_set_source_rgb(cr_context, bgcolor.r, bgcolor.g, bgcolor.b);
	cairo_paint(cr_context);

	drawBox(cr_context, rootBox.get());

	display->flip();

	fpsCounter++;
	uint64_t current_tick;
	HEL_CHECK(helGetClock(&current_tick));
	if(current_tick - fpsLastTick > 1000000000) {
		fpsCurrent = fpsCounter;
		printf("FPS %d\n", fpsCurrent);
		fpsCounter = 0;
		fpsLastTick = current_tick;
	}
}

void InitClosure::queriedBochs(HelHandle handle) {
	helx::Pipe device_pipe(handle);

	// acquire the device's resources
	HelError acquire_error;
	uint8_t acquire_buffer[128];
	size_t acquire_length;
	device_pipe.recvStringRespSync(acquire_buffer, 128, eventHub, 1, 0,
			acquire_error, acquire_length);
	HEL_CHECK(acquire_error);

	managarm::hw::PciDevice acquire_response;
	acquire_response.ParseFromArray(acquire_buffer, acquire_length);

	HelError bar_error;
	HelHandle bar_handle;
	device_pipe.recvDescriptorRespSync(eventHub, 1, 1, bar_error, bar_handle);
	HEL_CHECK(bar_error);
	
	// initialize graphics
	uintptr_t ports[] = { 0x1CE, 0x1CF, 0x1D0 };
	HelHandle io_handle;
	HEL_CHECK(helAccessIo(ports, 3, &io_handle));
	HEL_CHECK(helEnableIo(io_handle));

	writeReg(kRegEnable, 0); // disable the device
	writeReg(kRegXres, width);
	writeReg(kRegYres, height);
	writeReg(kRegBpp, kBpp32);
	writeReg(kRegEnable, kEnabled | kLinearFramebuffer);

	void *framebuffer;
	HEL_CHECK(helMapMemory(bar_handle, kHelNullHandle, nullptr,
			0, acquire_response.bars(0).length(), kHelMapReadWrite, &framebuffer));
	pixels = (uint8_t *)framebuffer;
	
	for(int y = 0; y < height; y++)
		for(int x = 0; x < width; x++)
			setPixel(x, y, 255, 255, 255);
	
//Freetype

	if(FT_Init_FreeType(&ftLibrary) != 0) {
		printf("FT_Init_FreeType() failed\n");
		abort();
	}
	
	const char *path = "/usr/share/fonts/SourceCodePro-Regular.otf";
	if(FT_New_Face(ftLibrary, path, 0, &ftFace) != 0) {
		printf("FT_New_Face() failed\n");
		abort();
	}
	
// cairo
	
	int stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, width);
	assert(stride == width * 4);
	display.buffers[0].crSurface = cairo_image_surface_create_for_data(pixels,
			CAIRO_FORMAT_RGB24, width, height, stride);
	
	display.buffers[0].crContext = cairo_create(display.buffers[0].crSurface);
	display.buffers[1].offsetY = 0;
	display.buffers[1].crSurface = cairo_image_surface_create_for_data(pixels + height * stride,
			CAIRO_FORMAT_RGB24, width, height, stride);
	display.buffers[1].crContext = cairo_create(display.buffers[1].crSurface);
	display.buffers[1].offsetY = height;
	
	crFont = cairo_ft_font_face_create_for_ft_face(ftFace, 0);

/*
	auto head = std::make_shared<Box>();
	head->fixedHeight = 20;
	head->backgroundColor = 0xCECECE;
	head->layout = kVerticalBlocks;

	head->widthType = Box::kSizeFillParent;
	head->heightType = Box::kSizeFixed;

	head->hasBorder = true;
	head->borderWidth = 1;
	head->borderColor = 0xAE3E17;
	
	auto headFont = std::make_shared<Box>();
	headFont->fixedHeight = 20;
	headFont->backgroundColor = 0xCECECE;
	headFont->layout = kLayoutLine;

	headFont->widthType = Box::kSizeFillParent;
	headFont->heightType = Box::kSizeFixed;

	headFont->hasText = true;
	headFont->text = "Tasty window";
	headFont->fontSize = 15.0;
	headFont->fontColor = 0x000000;

	head->appendChild(headFont);

	auto body = std::make_shared<Box>();
	body->fixedHeight = 580;
	body->backgroundColor = 0xCECECE;
	body->layout = kVerticalBlocks;
	
	body->widthType = Box::kSizeFillParent;
	body->heightType = Box::kSizeFixed;	
	
	body->hasBorder = true;
	body->borderWidth = 1;
	body->borderColor = 0xAE3E17;

	auto bodyFont = std::make_shared<Box>();
	bodyFont->fixedHeight = 580;
	bodyFont->backgroundColor = 0xCECECE;
	bodyFont->layout = kLayoutLine;
	
	headFont->widthType = Box::kSizeFillParent;
	headFont->heightType = Box::kSizeFixed;
	
	bodyFont->hasText = true;
	bodyFont->text = "Pudding lemon drops caramels liquorice fruitcake.";
	bodyFont->fontSize = 20.0;
	bodyFont->fontColor = 0x000000;

	body->appendChild(bodyFont);

*/

	rootBox = std::make_shared<Box>();
	rootBox->fixedHeight = 604;
	rootBox->fixedWidth = 900;
	rootBox->x = 20;
	rootBox->y = 20;
	rootBox->backgroundColor = 0xFFFFFF;
	rootBox->layout = kVerticalBlocks;	
	rootBox->widthType = Box::kSizeFixed;
	rootBox->heightType = Box::kSizeFixed;
	
	rootBox->hasBorder = true;
	rootBox->borderWidth = 2;
	rootBox->borderColor = 0xAE3E17;
	
	TerminalWidget widget(50, 10, 30.0);
	
	widget.setChar(0, 0, 'H');
	widget.setChar(1, 0, 'e');
	widget.setChar(2, 0, 'l');
	widget.setChar(3, 0, 'l');
	widget.setChar(4, 0, 'o');
	widget.setChar(0, 1, 'W');
	widget.setChar(1, 1, 'o');
	widget.setChar(2, 1, 'r');
	widget.setChar(3, 1, 'l');
	widget.setChar(4, 1, 'd');
	
	rootBox->appendChild(widget.getBox());
	layoutChildren(rootBox.get());
	
	for(int i = 0; true; i++)
		drawFrame(&display, i);
}

// --------------------------------------------------------
// main() function
// --------------------------------------------------------

int main() {
	printf("Starting Bochs VGA driver\n");

	auto closure = new InitClosure();
	(*closure)();

	while(true)
		eventHub.defaultProcessEvents();	
}

