
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

enum {
	kRegXres = 1,
	kRegYres = 2,
	kRegBpp = 3,
	kRegEnable = 4
};

enum {
	kBpp24 = 0x18,

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

enum ChildLayout {
	kChildNoLayout = 0,
	kChildHorizontalBlocks = 1,
	kChildVerticalBlocks = 2
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
	ChildLayout childLayout;

	Box *parent;
	std::vector<std::shared_ptr<Box>> children;
};

Box::Box() : hasBorder(false), borderWidth(0) { }

void Box::appendChild(std::shared_ptr<Box> child) {
	child->parent = this;
	children.push_back(child);
}

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
		box->actualWidth = box->parent->actualWidth - box->borderWidth * 2;
	}

	if(box->heightType == Box::kSizeFixed) {
		box->actualHeight = box->fixedHeight;
	}else if(box->heightType == Box::kSizeFillParent) {
		assert(box->parent->heightType == Box::kSizeFixed
				|| box->parent->heightType == Box::kSizeFillParent);
			box->actualHeight = box->parent->actualHeight - box->borderWidth * 2;
	}

	// for kSizeFixed and kSizeFillParent the size is computed at this point
	// kFitToChildren must be computed AFTER children are layouted

	if(box->childLayout == kChildNoLayout) {
		// do nothing
	}else if(box->childLayout == kChildVerticalBlocks) {
		int accumulated_y = 0;
		for(unsigned int i = 0; i < box->children.size(); i++) {
			auto child = box->children[i].get();
			child->x = box->x + child->borderWidth;
			child->y = box->y + accumulated_y + child->borderWidth;
			
			assert(child->heightType == Box::kSizeFixed);
			layoutChildren(child);
			accumulated_y += child->actualHeight + child->borderWidth * 2;
		}
	}else if(box->childLayout == kChildHorizontalBlocks) {
		int accumulated_x = 0;
		for(unsigned int i = 0; i < box->children.size(); i++) {
			auto child = box->children[i].get();
			child->y = box->y + child->borderWidth;
			child->x = box->x + accumulated_x + child->borderWidth;

			assert(child->widthType == Box::kSizeFixed);
			layoutChildren(child);
			accumulated_x += child->actualWidth + child->borderWidth * 2;
		}
	}else{
		assert(!"Illegal ChildLayout!");
	}

	if(box->widthType == Box::kSizeFitToChildren) {
		int child_width = 0;
		for(unsigned int i = 0; i < box->children.size(); i++) {
			auto child = box->children[i].get();
			child_width += child->actualWidth + child->borderWidth * 2;
		}
		box->actualWidth = child_width;
	}
	
	if(box->heightType == Box::kSizeFitToChildren) {
		int child_height = 0;
		for(unsigned int i = 0; i < box->children.size(); i++) {
			auto child = box->children[i].get();
			child_height += child->actualHeight + child->borderWidth * 2;
		}
		box->actualHeight = child_height;
	}
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
	pixels[(y * width + x) * 3] = b;
	pixels[(y * width + x) * 3 + 1] = g;
	pixels[(y * width + x) * 3 + 2] = r;
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
	writeReg(kRegBpp, kBpp24);
	writeReg(kRegEnable, kEnabled | kLinearFramebuffer);

	void *framebuffer;
	HEL_CHECK(helMapMemory(bar_handle, kHelNullHandle, nullptr,
			acquire_response.bars(0).length(), kHelMapReadWrite, &framebuffer));
	pixels = (uint8_t *)framebuffer;
	
	for(int y = 0; y < height; y++)
		for(int x = 0; x < width; x++)
			setPixel(x, y, 255, 255, 255);

//Freetype

	FT_Library library;
	if(FT_Init_FreeType(&library) != 0) {
		printf("FT_Init_FreeType() failed\n");
		abort();
	}
	
	const char *path = "initrd/SourceCodePro-Regular.otf";

	HelHandle image_handle;
	size_t image_size;
	void *image_ptr;
	HEL_CHECK(helRdOpen(path, strlen(path), &image_handle));
	HEL_CHECK(helMemoryInfo(image_handle, &image_size));
	HEL_CHECK(helMapMemory(image_handle, kHelNullHandle, nullptr, image_size,
			kHelMapReadOnly, &image_ptr));
	
	FT_Face face;
	if(FT_New_Memory_Face(library, (FT_Byte *)image_ptr, image_size, 0, &face) != 0) {
		printf("FT_New_Memory_Face() failed\n");
		abort();
	}
		
	if(FT_Set_Pixel_Sizes(face, 32, 0) != 0) {
		printf("FT_Set_Pixel_Sizes() failed\n");
		abort();
	}
	
	const char *text = "managarm + Bochs VGA";
	int x = 64, base_y = 64;
	for(size_t i = 0; i < strlen(text); i++) {
		FT_UInt glyph_index = FT_Get_Char_Index(face, text[i]);
		if(glyph_index == 0) {
			printf("FT_Get_Char_Index() failed\n");
			abort();
		}

		if(FT_Load_Glyph(face, glyph_index, FT_LOAD_DEFAULT) != 0) {
			printf("FT_Load_Glyph() failed\n");
			abort();
		}

		if(FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL) != 0) {
			printf("FT_Render_Glyph() failed\n");
			abort();
		}

		FT_Bitmap &bitmap = face->glyph->bitmap;
		assert(bitmap.pixel_mode == FT_PIXEL_MODE_GRAY);
		for(unsigned int gy = 0; gy < bitmap.rows; gy++)
			for(unsigned int gx = 0; gx < bitmap.width; gx++) {
				uint8_t value = 255 - bitmap.buffer[gy * bitmap.pitch + gx];
				setPixel(x + face->glyph->bitmap_left + gx,
						base_y - face->glyph->bitmap_top + gy, value, value, value);
			}

		x += face->glyph->advance.x >> 6;
	}

	printf("FT Success!\n");


//cairo
	cairo_surface_t *surface;
	cairo_t *cr;

	surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, width, height);
	cr = cairo_create(surface);

	auto child1 = std::make_shared<Box>();
	child1->fixedWidth = 50;
	child1->backgroundColor = kSolarMargenta;
	child1->childLayout = kChildNoLayout;

	child1->widthType = Box::kSizeFixed;
	child1->heightType = Box::kSizeFillParent;

	child1->hasBorder = true;
	child1->borderWidth = 15;
	child1->borderColor = 0x3E3E3E;
	
	auto child2 = std::make_shared<Box>();
	child2->fixedWidth = 100;
	child2->backgroundColor = kSolarYellow;
	child2->childLayout = kChildNoLayout;

	child2->widthType = Box::kSizeFixed;
	child2->heightType = Box::kSizeFillParent;

	child2->hasBorder = true;
	child2->borderWidth = 30;
	child2->borderColor = kSolarGreen;

	auto child3 = std::make_shared<Box>();
	child3->fixedWidth = 200;
	child3->backgroundColor = kSolarBlue;
	child3->childLayout = kChildNoLayout;

	child3->widthType = Box::kSizeFixed;
	child3->heightType = Box::kSizeFillParent;

	Box box;
	box.fixedHeight = 250;
	box.x = 99;
	box.y = 11;
	box.backgroundColor = kSolarCyan;
	box.childLayout = kChildHorizontalBlocks;

	box.widthType = Box::kSizeFitToChildren;
	box.heightType = Box::kSizeFixed;

	box.hasBorder = true;
	box.borderWidth = 20;
	box.borderColor = 0xCECECE;
	
	box.appendChild(child1);
	box.appendChild(child2);
	box.appendChild(child3);

	layoutChildren(&box);

	drawBox(cr, &box);

	cairo_surface_flush(surface);
	int stride = cairo_image_surface_get_stride(surface);
	uint8_t *data = cairo_image_surface_get_data(surface);
	assert(data);
	for(int y = 0; y < height; y++)
		for(int x = 0; x < width; x++) {
			uint8_t b = data[y * stride + x * 4];
			uint8_t g = data[y * stride + x * 4 + 1];
			uint8_t r = data[y * stride + x * 4 + 2];
			setPixel(x, y, r, g, b);
		}
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

