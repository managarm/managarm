
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

#include <hel.h>
#include <hel-syscalls.h>
#include <helx.hpp>

#include <frigg/atomic.hpp>

#include <mbus.pb.h>
#include <hw.pb.h>

//#include "pci.hpp"

//#include <ft2build.h>
//#include FT_FREETYPE_H

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
helx::Pipe mbusPipe;

int main() {
	printf("Starting Bochs VGA driver\n");

	// connect to mbus
	const char *mbus_path = "config/mbus";
	HelHandle mbus_handle;
	HEL_CHECK(helRdOpen(mbus_path, strlen(mbus_path), &mbus_handle));
	helx::Client mbus_connect(mbus_handle);
	
	HelError mbus_connect_error;
	mbus_connect.connectSync(eventHub, mbus_connect_error, mbusPipe);
	HEL_CHECK(mbus_connect_error);

	// query the bochs vga PCI device
	managarm::mbus::CntRequest request;
	request.set_req_type(managarm::mbus::CntReqType::QUERY_IF);
	request.set_object_id(5);

	std::string serialized;
	request.SerializeToString(&serialized);
	mbusPipe.sendStringReq(serialized.data(), serialized.size(), 1, 0);

	HelError device_error;
	HelHandle device_handle;
	mbusPipe.recvDescriptorRespSync(eventHub, 1, 1, device_error, device_handle);
	HEL_CHECK(device_error);
	helx::Pipe device_pipe(device_handle);

	// acquire the device's resources
	HelError acquire_error;
	uint8_t buffer[128];
	size_t length;
	device_pipe.recvStringRespSync(buffer, 128, eventHub, 1, 0, acquire_error, length);
	HEL_CHECK(acquire_error);

	managarm::hw::PciDevice response;
	response.ParseFromArray(buffer, length);

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
			response.bars(0).length(), kHelMapReadWrite, &framebuffer));
	pixels = (uint8_t *)framebuffer;
	
	for(int y = 0; y < height; y++)
		for(int x = 0; x < width; x++)
			setPixel(x, y, 255, 255, 255);
	
/*	FT_Library library;
	if(FT_Init_FreeType(&library) != 0) {
		printf("FT_Init_FreeType() failed\n");
		abort();
	}
	
	const char *path = "initrd/DejaVuSansMono.ttf";

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

		assert(face->glyph->bitmap.pixel_mode == FT_PIXEL_MODE_GRAY);
		FT_Bitmap &bitmap = face->glyph->bitmap;
		for(unsigned int gy = 0; gy < bitmap.rows; gy++)
			for(unsigned int gx = 0; gx < bitmap.width; gx++) {
				uint8_t value = 255 - bitmap.buffer[gy * bitmap.pitch + gx];
				setPixel(x + face->glyph->bitmap_left + gx,
						base_y - face->glyph->bitmap_top + gy, value, value, value);
			}

		x += face->glyph->advance.x >> 6;
	}

	printf("FT Success!\n");*/
}

