#include <core/drm/debug.hpp>
#include <lil/intel.h>

#include "debug.hpp"
#include "gfx.hpp"

GfxDevice::GfxDevice(protocols::hw::Device hw_device, uint16_t pch_devid)
	: _hwDevice{std::move(hw_device)}, _vramAllocator(26, 12), _pchDevId{pch_devid} {

};

async::result<std::unique_ptr<drm_core::Configuration>> GfxDevice::initialize() {
	std::vector<drm_core::Assignment> assignments;

	co_await _hwDevice.claimDevice();

	auto config = createConfiguration();
	auto state = atomicState();
	assert(config->capture(assignments, state));
	config->commit(std::move(state));

	co_return std::move(config);
}

std::unique_ptr<drm_core::Configuration> GfxDevice::createConfiguration() {
	return std::make_unique<Configuration>(this);
}

std::pair<std::shared_ptr<drm_core::BufferObject>, uint32_t> GfxDevice::createDumb(uint32_t width,
		uint32_t height, uint32_t bpp) {
	size_t pitch = ((width * (bpp / 8) + 63) & ~0x3F);
	size_t size = height * pitch;

	auto offset = _vramAllocator.allocate(size);
	auto buffer = std::make_shared<GfxDevice::BufferObject>(this, offset, width, height, size);

	auto mapping = installMapping(buffer.get());
	buffer->setupMapping(mapping);
	return std::make_pair(std::move(buffer), pitch);
}

std::shared_ptr<drm_core::FrameBuffer> GfxDevice::createFrameBuffer(std::shared_ptr<drm_core::BufferObject> base_bo,
		uint32_t width, uint32_t height, uint32_t format, uint32_t pitch) {
	auto bo = std::static_pointer_cast<GfxDevice::BufferObject>(base_bo);
	auto f = drm_core::getFormatInfo(format);
	assert(f);
	auto bpp = drm_core::getFormatBpp(*f, 0);

	auto pixel_pitch = pitch / (bpp / 8);
	assert(pixel_pitch >= width);

	assert(bo->getSize() >= pitch * height);

	auto fb = std::make_shared<FrameBuffer>(this, bo, pixel_pitch);
	fb->setupWeakPtr(fb);
	registerObject(fb.get());

	fb->setFormat(format);

	return fb;
}

//returns major, minor, patchlvl
std::tuple<int, int, int> GfxDevice::driverVersion() {
	return {1, 0, 0};
}

//returns name, desc, date
std::tuple<std::string, std::string, std::string> GfxDevice::driverInfo() {
	return { "intel-lil", "Intel GPU driver based on lil", "0"};
}

bool GfxDevice::Configuration::capture(std::vector<drm_core::Assignment> assignment, std::unique_ptr<drm_core::AtomicState> &state) {
	if(logDrmRequests)
		std::cout << "gfx/intel-lil: Configuration capture" << std::endl;

	for(auto &assign: assignment) {
		assert(assign.property->validate(assign));
		assign.property->writeToState(assign, state);

		if(!logDrmRequests)
			continue;

		auto mode_obj = assign.object;

		switch(mode_obj->type()) {
			case drm_core::ObjectType::crtc:
				std::cout << "\tCRTC (ID " << mode_obj->id() << ")" << std::endl;
				break;
			case drm_core::ObjectType::connector:
				std::cout << "\tConnector (ID " << mode_obj->id() << ")" << std::endl;
				break;
			case drm_core::ObjectType::encoder:
				std::cout << "\tEncoder (ID " << mode_obj->id() << ")" << std::endl;
				break;
			case drm_core::ObjectType::frameBuffer:
				std::cout << "\tFB (ID " << mode_obj->id() << ")" << std::endl;
				break;
			case drm_core::ObjectType::plane:
				std::cout << "\tPlane (ID " << mode_obj->id() << ")" << std::endl;
				break;
		}

		auto prop_type = assign.property->propertyType();

		if(std::holds_alternative<drm_core::IntPropertyType>(prop_type)) {
			std::cout << "\t\t" << assign.property->name() << " = " << assign.intValue << " (int)" << std::endl;
		} else if(std::holds_alternative<drm_core::EnumPropertyType>(prop_type)) {
			std::cout << "\t\t" << assign.property->name() << " = " << assign.intValue << " " << assign.property->enumInfo().at(assign.intValue) << " (enum)" << std::endl;
		} else if(std::holds_alternative<drm_core::BlobPropertyType>(prop_type)) {
			std::cout << "\t\t" << assign.property->name() << " = " << (assign.blobValue ? std::to_string(assign.blobValue->id()) : "<none>") << " (blob)" << std::endl;
		} else if(std::holds_alternative<drm_core::ObjectPropertyType>(prop_type)) {
			std::cout << "\t\t" << assign.property->name() << " = " << (assign.objectValue ? std::to_string(assign.objectValue->id()) : "<none>") << " (modeobject)" << std::endl;
		}
	}

	return true;
}

void GfxDevice::Configuration::dispose() {

}

void GfxDevice::Configuration::commit(std::unique_ptr<drm_core::AtomicState> state) {
	_doCommit(std::move(state));
}

async::detached GfxDevice::Configuration::_doCommit(std::unique_ptr<drm_core::AtomicState> state) {
	complete();

	co_return;
}

GfxDevice::FrameBuffer::FrameBuffer(GfxDevice *device, std::shared_ptr<GfxDevice::BufferObject> bo, uint32_t pixel_pitch)
		: drm_core::FrameBuffer { device, device->allocator.allocate() } {
	_bo = bo;
	_pixelPitch = pixel_pitch;
}

void GfxDevice::FrameBuffer::notifyDirty() {

}

uint32_t GfxDevice::FrameBuffer::getWidth() {
	return _bo->getWidth();
}

uint32_t GfxDevice::FrameBuffer::getHeight() {
	return _bo->getHeight();
}

GfxDevice::BufferObject *GfxDevice::FrameBuffer::getBufferObject() {
	return _bo.get();
}

GfxDevice::BufferObject::BufferObject(GfxDevice *device, GpuAddr gpu_addr, uint32_t width, uint32_t height, size_t size)
: drm_core::BufferObject{width, height}, _device{device}, _gpuAddr{gpu_addr}, _size{size} {
	_size = (_size + (4096 - 1)) & ~(4096 - 1);

	HelHandle handle;
	HEL_CHECK(helAllocateMemory(size, 0, nullptr, &handle));
	HEL_CHECK(helMapMemory(handle, kHelNullHandle, nullptr, 0, _size, kHelMapProtRead, &_ptr));
	_memoryView = helix::UniqueDescriptor{handle};
};

GpuAddr GfxDevice::BufferObject::getAddress() {
	return _gpuAddr;
}

std::shared_ptr<drm_core::BufferObject> GfxDevice::BufferObject::sharedBufferObject() {
	return this->shared_from_this();
}

size_t GfxDevice::BufferObject::getSize() {
	return _size;
}

std::pair<helix::BorrowedDescriptor, uint64_t> GfxDevice::BufferObject::getMemory() {
	return std::make_pair(helix::BorrowedDescriptor{_memoryView}, 0);
}
