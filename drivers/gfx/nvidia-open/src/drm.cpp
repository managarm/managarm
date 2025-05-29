#include <string.h>
#include <print>

#include "gfx.hpp"
#include "utils.hpp"

extern "C" {

#include <nvkms-kapi.h>

}

extern const struct NvKmsKapiFunctionsTable* nvKms;

std::unique_ptr<drm_core::Configuration> GfxDevice::createConfiguration() {
	return std::make_unique<Configuration>(this);
}

std::pair<std::shared_ptr<drm_core::BufferObject>, uint32_t> GfxDevice::createDumb(uint32_t width,
	uint32_t height, uint32_t bpp) {
	auto alignedPitch = (((width * ((bpp + 7) >> 3)) + (pitchAlignment_ - 1)) / pitchAlignment_) * pitchAlignment_;
	size_t size = height * alignedPitch;
	size = ((size + 0xFFF) / 0x1000) * 0x1000;

	assert(hasVideoMemory_);
	uint8_t compressible = 0;

	auto mem = nvKms->allocateVideoMemory(kmsdev,
		NvKmsSurfaceMemoryLayoutPitch,
		NVKMS_KAPI_ALLOCATION_TYPE_SCANOUT,
		size,
		&compressible);
	assert(mem);

	uintptr_t physicalAddress = 0;
	nvKms->mapMemory(kmsdev, mem,
		NVKMS_KAPI_MAPPING_TYPE_USER,
		reinterpret_cast<void **>(&physicalAddress));

	auto barIndex = getNvidiaBarIndex(NV_GPU_BAR_INDEX_FB);
	auto offset = physicalAddress - info_.barInfo[barIndex].address;

	auto bo = std::make_shared<BufferObject>(this, size, apertureHandle_.dup(), offset, mem, width, height);
	auto mapping = installMapping(bo.get());
	bo->setupMapping(mapping);
	bos_.push_back(bo);
	return {bo, alignedPitch};
}

std::shared_ptr<drm_core::FrameBuffer>
GfxDevice::createFrameBuffer(std::shared_ptr<drm_core::BufferObject> buff,
		uint32_t width, uint32_t height, uint32_t fourcc, uint32_t pitch) {
	NvKmsKapiCreateSurfaceParams params = {};
	auto bo = std::static_pointer_cast<BufferObject>(buff);

	switch(fourcc) {
		case DRM_FORMAT_ARGB8888: params.format = NvKmsSurfaceMemoryFormatA8R8G8B8; break;
		case DRM_FORMAT_XRGB8888: params.format = NvKmsSurfaceMemoryFormatX8R8G8B8; break;
		case DRM_FORMAT_XBGR8888: params.format = NvKmsSurfaceMemoryFormatX8B8G8R8; break;
		default:
			std::println("gfx/nvidia-open: unhandled DRM format 0x{:08x}", fourcc);
			assert(!"unimplemented");
	}

	params.planes[0].memory = bo->memHandle();
	params.planes[0].offset = 0;
	params.planes[0].pitch = pitch;
	params.height = height;
	params.width = width;
	params.noDisplayCaching = NV_TRUE;
	params.explicit_layout = NV_FALSE;

	auto pSurface = nvKms->createSurface(kmsdev, &params);
	assert(pSurface);

	auto fb = std::make_shared<FrameBuffer>(this, bo, pitch, pSurface);
	fb->setupWeakPtr(fb);
	registerObject(fb.get());
	return fb;
}

std::tuple<int, int, int> GfxDevice::driverVersion() {
	return {0, 0, 0};
}
std::tuple<std::string, std::string, std::string> GfxDevice::driverInfo() {
	return {"nvidia-drm", "NVIDIA DRM driver (managarm)", "0.0.0"};
}

// ----------------------------------------------------------------
// GfxDevice::Plane
// ----------------------------------------------------------------

GfxDevice::Plane::Plane(GfxDevice *dev, PlaneType type, size_t layerIndex)
	: drm_core::Plane { dev, dev->allocator.allocate(), type }, layerIndex_{layerIndex} {
}

uint32_t GfxDevice::Plane::supportedRotations(NvKmsKapiDeviceResourcesInfo &info, size_t layer) {
	uint32_t supportedRotations = 0;
	struct NvKmsRRParams rrParams = {
		.rotation = NVKMS_ROTATION_0,
		.reflectionX = true,
		.reflectionY = true,
	};

	if((NVBIT(NvKmsRRParamsToCapBit(&rrParams)) & info.caps.layer[layer].validRRTransforms) != 0) {
		supportedRotations |= DRM_MODE_REFLECT_X;
		supportedRotations |= DRM_MODE_REFLECT_Y;
	}

	rrParams.reflectionX = false;
	rrParams.reflectionY = false;

	std::array<NvKmsRotation, 4> validRotations = {
		NVKMS_ROTATION_0,
		NVKMS_ROTATION_90,
		NVKMS_ROTATION_180,
		NVKMS_ROTATION_270,
	};

	for(auto curRotation : validRotations) {
		rrParams.rotation = curRotation;
		if((NVBIT(NvKmsRRParamsToCapBit(&rrParams)) & info.caps.layer[layer].validRRTransforms) == 0)
			continue;

		switch (curRotation) {
			case NVKMS_ROTATION_0:
				supportedRotations |= DRM_MODE_ROTATE_0;
				break;
			case NVKMS_ROTATION_90:
				supportedRotations |= DRM_MODE_ROTATE_90;
				break;
			case NVKMS_ROTATION_180:
				supportedRotations |= DRM_MODE_ROTATE_180;
				break;
			case NVKMS_ROTATION_270:
				supportedRotations |= DRM_MODE_ROTATE_270;
				break;
			default:
				break;
		}
	}

	return supportedRotations;
}

// ----------------------------------------------------------------
// GfxDevice::Connector
// ----------------------------------------------------------------

GfxDevice::Connector::Connector(GfxDevice *dev, NvKmsConnectorType type, bool internal,
		size_t physicalIndex, char dpAddress[NVKMS_DP_ADDRESS_STRING_LENGTH])
		: drm_core::Connector { dev, dev->allocator.allocate() },
		_device{dev}, type_{type}, internal_{internal}, physicalIndex_{physicalIndex} {
	strcpy(dpAddress_, dpAddress);
}

std::shared_ptr<GfxDevice::Connector> GfxDevice::Connector::find(GfxDevice *dev, size_t physicalIndex,
		NvKmsConnectorType type, bool internal, char dpAddress[NVKMS_DP_ADDRESS_STRING_LENGTH]) {
	for(auto c : dev->connectors_) {
		if(c->physicalIndex() == physicalIndex
		&& c->type() == type
		&& c->internal() == internal
		&& !strncmp(c->dpAddress().data(), dpAddress, NVKMS_DP_ADDRESS_STRING_LENGTH))
			return c;
	}

	auto connector = std::make_shared<Connector>(dev, type, internal, physicalIndex, dpAddress);
	connector->setupWeakPtr(connector);
	connector->setupState(connector);
	connector->setConnectorType(Connector::getConnectorType(type, internal));

	dev->registerObject(connector.get());
	dev->attachConnector(connector.get());

	dev->connectors_.push_back(connector);

	return connector;
}

void GfxDevice::Connector::updateModeList() {
	NvU32 modeIndex = 0;
	size_t mmWidth = 0, mmHeight = 0;
	std::vector<drm_mode_modeinfo> modes;

	while(true) {
        NvKmsKapiDisplayMode displayMode;
        NvBool valid = 0;
        NvBool preferredMode = NV_FALSE;

        int ret = nvKms->getDisplayMode(_device->kmsdev, detectedEncoder->handle(),
			modeIndex++, &displayMode, &valid, &preferredMode);

        if(ret < 0) {
            std::println("Failed to get mode at modeIndex {} of NvKmsKapiDisplay 0x{:08x}",
                modeIndex, detectedEncoder->handle());
            break;
        }

        /* Is end of mode-list */
        if(ret == 0)
            break;

        /* Ignore invalid modes */
        if(!valid)
            continue;

		drm_mode_modeinfo mi{
			.type = DRM_MODE_TYPE_DRIVER,
		};

		utils::toDrmModeInfo(&displayMode, &mi);

		if(preferredMode) {
			mi.type |= DRM_MODE_TYPE_PREFERRED;
			mmWidth = displayMode.timings.widthMM;
			mmHeight = displayMode.timings.heightMM;
		}

		modes.push_back(mi);
	}

	if(!modes.empty()) {
		setupPhysicalDimensions(mmWidth, mmHeight);
	}

	setModeList(modes);
}

NvKmsKapiDynamicDisplayParams *GfxDevice::Connector::updateDisplayInfo(NvKmsKapiDisplay handle) {
	auto params = reinterpret_cast<NvKmsKapiDynamicDisplayParams *>(calloc(1, sizeof(NvKmsKapiDynamicDisplayParams)));
	if(!params)
		return nullptr;

	params->handle = handle;

	auto success = nvKms->getDynamicDisplayInfo(_device->kmsdev, params);
	if(!success)
		return nullptr;

	return params;
}

async::result<void> GfxDevice::Connector::probe() {
	for(auto e : this->getPossibleEncoders()) {
		auto enc = static_cast<Encoder *>(e);

		auto params = updateDisplayInfo(enc->handle());
		if(!params)
			continue;

		detectedEncoder = enc;
		setCurrentEncoder(enc);
		updateModeList();

		if(params->connected) {
			setCurrentStatus(1);
			co_return;
		}
	}

	detectedEncoder = nullptr;

	setCurrentStatus(2);
	co_return;
}

// ----------------------------------------------------------------
// GfxDevice::Encoder
// ----------------------------------------------------------------

GfxDevice::Encoder::Encoder(GfxDevice *dev, NvKmsKapiDisplay handle)
	: drm_core::Encoder { dev, dev->allocator.allocate() }, handle_{handle} {
}

// ----------------------------------------------------------------
// GfxDevice::Crtc
// ----------------------------------------------------------------

GfxDevice::Crtc::Crtc(GfxDevice *dev, size_t headId, std::shared_ptr<GfxDevice::Plane> primary)
		: drm_core::Crtc { dev, dev->allocator.allocate() },
		headId_{headId}, primaryPlane_{std::move(primary)} {
	_device = dev;
}

drm_core::Plane *GfxDevice::Crtc::primaryPlane() {
	return primaryPlane_.get();
}

drm_core::Plane *GfxDevice::Crtc::cursorPlane() {
	return nullptr;
}

// ----------------------------------------------------------------
// GfxDevice::BufferObject
// ----------------------------------------------------------------

GfxDevice::BufferObject::BufferObject(GfxDevice *dev, size_t size, helix::UniqueDescriptor mem, size_t memOffset,
	NvKmsKapiMemory *memHandle, uint32_t width, uint32_t height)
: drm_core::BufferObject{width, height}, _size{size},
_mem{std::move(mem)}, _memOffset{memOffset}, _memHandle{memHandle} {
	(void) dev;
}

std::shared_ptr<drm_core::BufferObject> GfxDevice::BufferObject::sharedBufferObject() {
	return this->shared_from_this();
}

size_t GfxDevice::BufferObject::getSize() {
	return _size;
}

std::pair<helix::BorrowedDescriptor, uint64_t> GfxDevice::BufferObject::getMemory() {
	return std::make_pair(helix::BorrowedDescriptor{_mem}, _memOffset);
}

// ----------------------------------------------------------------
// GfxDevice::FrameBuffer
// ----------------------------------------------------------------

GfxDevice::FrameBuffer::FrameBuffer(GfxDevice *dev, std::shared_ptr<GfxDevice::BufferObject> bo,
	uint32_t pixel_pitch, NvKmsKapiSurface *surface)
: drm_core::FrameBuffer { dev, dev->allocator.allocate() },
_bo{std::move(bo)}, _pixelPitch{pixel_pitch}, _surface{surface} {

}

GfxDevice::BufferObject *GfxDevice::FrameBuffer::getBufferObject() {
	return _bo.get();
}

uint32_t GfxDevice::FrameBuffer::getPixelPitch() {
	return _pixelPitch;
}

void GfxDevice::FrameBuffer::notifyDirty() {

}

uint32_t GfxDevice::FrameBuffer::getWidth() {
	return _bo->getWidth();
}

uint32_t GfxDevice::FrameBuffer::getHeight() {
	return _bo->getHeight();
}

// ----------------------------------------------------------------
// GfxDevice::Configuration
// ----------------------------------------------------------------

bool GfxDevice::Configuration::capture(std::vector<drm_core::Assignment> assignment, std::unique_ptr<drm_core::AtomicState> &state) {
	for(auto &assign : assignment) {
		assert(assign.property->validate(assign));
		assign.property->writeToState(assign, state);
	}

	NvKmsKapiRequestedModeSetConfig testconfig;
	memset(&testconfig, 0, sizeof(testconfig));

	for(auto [id, cs] : state->crtc_states()) {
		auto crtcState = std::static_pointer_cast<Crtc::CrtcState>(cs);

		if(crtcState->modeChanged) {
			utils::toNvModeInfo(
				reinterpret_cast<const drm_mode_modeinfo *>(crtcState->mode->data()),
				&crtcState->params.modeSetConfig.mode);
		}

		crtcState->params.flags.modeChanged = crtcState->modeChanged;

		bool connectorsChanged = false;

		for(auto [id, cs] : state->connector_states()) {
			auto oldcs = _device->atomicState()->connector(id);

			if(oldcs->crtc != cs->crtc) {
				connectorsChanged = true;
				break;
			}
		}

		if(connectorsChanged) {
			crtcState->params.modeSetConfig.numDisplays = 0;
			memset(crtcState->params.modeSetConfig.displays, 0, sizeof(crtcState->params.modeSetConfig.displays));

			for(auto [conId, cs] : state->connector_states()) {
				if(!cs->crtc || cs->crtc->id() != id)
					continue;

				auto con = std::static_pointer_cast<Connector>(cs->connector);
				auto encoder = std::static_pointer_cast<Encoder>(con->currentEncoder()->sharedModeObject());

				auto valid = nvKms->validateDisplayMode(_device->kmsdev, encoder->handle(), &crtcState->params.modeSetConfig.mode);
				assert(valid);

				crtcState->params.modeSetConfig.displays[crtcState->params.modeSetConfig.numDisplays++] = encoder->handle();
			}
		}

		crtcState->params.flags.displaysChanged = connectorsChanged;

		if(crtcState->activeChanged)
			crtcState->params.modeSetConfig.bActive = crtcState->active;

		crtcState->params.flags.activeChanged = crtcState->activeChanged;

		crtcState->params.modeSetConfig.vrrEnabled = NV_FALSE;
		crtcState->params.modeSetConfig.olutFpNormScale = NVKMS_OLUT_FP_NORM_SCALE_DEFAULT;

		auto crtc = std::static_pointer_cast<Crtc>(crtcState->crtc().lock());

		for(auto plane : _device->planes_) {
			auto ps = state->plane(plane->id());

			if(ps->crtc != crtc)
				continue;

			auto fb = static_cast<FrameBuffer *>(plane->getFrameBuffer());
			auto layerConfig = &crtcState->params.layerRequestedConfig[plane->layerIndex()];

			if(!fb) {
				memset(layerConfig, 0, sizeof(crtcState->params.layerRequestedConfig[plane->layerIndex()]));

				layerConfig->config.csc = NVKMS_IDENTITY_CSC_MATRIX;
				layerConfig->flags.surfaceChanged = NV_TRUE;
				layerConfig->flags.srcXYChanged = NV_TRUE;
				layerConfig->flags.srcWHChanged = NV_TRUE;
				layerConfig->flags.dstXYChanged = NV_TRUE;
				layerConfig->flags.dstWHChanged = NV_TRUE;

				continue;
			}

			auto oldps = _device->atomicState()->plane(plane->id());

			layerConfig->config.surface = fb->surface();
			layerConfig->config.srcX = ps->src_x;
			layerConfig->config.srcY = ps->src_y;
			layerConfig->config.srcWidth  = ps->src_w;
			layerConfig->config.srcHeight = ps->src_h;

			layerConfig->config.dstX = ps->crtc_x;
			layerConfig->config.dstY = ps->crtc_y;
			layerConfig->config.dstWidth  = ps->crtc_w;
			layerConfig->config.dstHeight = ps->crtc_h;

			layerConfig->config.rrParams.rotation = NVKMS_ROTATION_0;
			layerConfig->flags.surfaceChanged = NV_TRUE;

			if(!oldps->plane && oldps->plane != ps->plane) {
				layerConfig->flags.srcXYChanged = NV_TRUE;
				layerConfig->flags.srcWHChanged = NV_TRUE;
				layerConfig->flags.dstXYChanged = NV_TRUE;
				layerConfig->flags.dstWHChanged = NV_TRUE;
			} else {
				layerConfig->flags.srcXYChanged = NV_FALSE;
				layerConfig->flags.srcWHChanged = NV_FALSE;
				layerConfig->flags.dstXYChanged = NV_FALSE;
				layerConfig->flags.dstWHChanged = NV_FALSE;
			}

			layerConfig->config.csc = NVKMS_IDENTITY_CSC_MATRIX;
			layerConfig->config.minPresentInterval = 1;
			layerConfig->config.tearing = NV_FALSE;

			layerConfig->config.compParams.compMode = NVKMS_COMPOSITION_BLENDING_MODE_OPAQUE;
			layerConfig->config.inputColorSpace = NVKMS_INPUT_COLOR_SPACE_NONE;
			layerConfig->config.inputColorRange = NVKMS_INPUT_COLOR_RANGE_DEFAULT;
			layerConfig->config.inputTf = NVKMS_INPUT_TF_LINEAR;
			layerConfig->config.hdrMetadata.enabled = false;
			layerConfig->config.outputTf = NVKMS_OUTPUT_TF_NONE;
			layerConfig->config.syncParams.preSyncptSpecified = false;
			layerConfig->config.syncParams.postSyncptRequested = false;
			layerConfig->config.syncParams.semaphoreSpecified = false;

			for(size_t layer = 1; layer < 8; layer++) {
				memset(&crtcState->params.layerRequestedConfig[layer].config, 0, sizeof(crtcState->params.layerRequestedConfig[layer].config));
				crtcState->params.layerRequestedConfig[layer].config.csc = NVKMS_IDENTITY_CSC_MATRIX;
			}
		}

		testconfig.headRequestedConfig[crtc->headId()] = crtcState->params;
        testconfig.headsMask |= 1 << crtc->headId();
	}

	NvKmsKapiModeSetReplyConfig reply_config = {};
	return nvKms->applyModeSetConfig(_device->kmsdev, &testconfig, &reply_config, false);
}

void GfxDevice::Configuration::dispose() {

}

void GfxDevice::Configuration::commit(std::unique_ptr<drm_core::AtomicState> state) {
	for(auto [id, cs] : state->crtc_states()) {
		auto obj = _device->findObject(id);
		assert(obj);
		auto baseCrtc = obj->asCrtc();
		assert(baseCrtc);
		baseCrtc->setDrmState(cs);
	}

	for(auto [id, cs] : state->connector_states()) {
		auto obj = _device->findObject(id);
		assert(obj);
		auto baseConnector = obj->asConnector();
		assert(baseConnector);
		baseConnector->setDrmState(cs);
	}

	for(auto [id, cs] : state->plane_states()) {
		auto obj = _device->findObject(id);
		assert(obj);
		auto basePlane = obj->asPlane();
		assert(basePlane);
		basePlane->setDrmState(cs);
	}

	dispatch(std::move(state));
}

async::detached GfxDevice::Configuration::dispatch(std::unique_ptr<drm_core::AtomicState> state) {
	NvKmsKapiRequestedModeSetConfig config;
	memset(&config, 0, sizeof(config));

	for(size_t i = 0; i < 4; i++) {
		config.headRequestedConfig[i].layerRequestedConfig[0].config.csc = NVKMS_IDENTITY_CSC_MATRIX;
		config.headRequestedConfig[i].layerRequestedConfig[1].config.csc = NVKMS_IDENTITY_CSC_MATRIX;
		config.headRequestedConfig[i].layerRequestedConfig[2].config.csc = NVKMS_IDENTITY_CSC_MATRIX;
		config.headRequestedConfig[i].layerRequestedConfig[3].config.csc = NVKMS_IDENTITY_CSC_MATRIX;
		config.headRequestedConfig[i].layerRequestedConfig[4].config.csc = NVKMS_IDENTITY_CSC_MATRIX;
		config.headRequestedConfig[i].layerRequestedConfig[5].config.csc = NVKMS_IDENTITY_CSC_MATRIX;
		config.headRequestedConfig[i].layerRequestedConfig[6].config.csc = NVKMS_IDENTITY_CSC_MATRIX;
		config.headRequestedConfig[i].layerRequestedConfig[7].config.csc = NVKMS_IDENTITY_CSC_MATRIX;
	}

	auto flipWait = false;

	for(auto [id, cs] : state->crtc_states()) {
		auto crtc = std::static_pointer_cast<Crtc>(cs->crtc().lock());
		if(!crtc)
			continue;

		auto crtcState = std::static_pointer_cast<Crtc::CrtcState>(cs);
		config.headRequestedConfig[crtc->headId()] = crtcState->params;

		// TODO: support planes other than the primary
		if(!config.headRequestedConfig[crtc->headId()].flags.modeChanged
		&& config.headRequestedConfig[crtc->headId()].layerRequestedConfig[0].flags.surfaceChanged) {
			flipWait = true;
		}

        config.headsMask |= 1 << crtc->headId();
	}

	NvKmsKapiModeSetReplyConfig reply_config = {};
	auto success = nvKms->applyModeSetConfig(_device->kmsdev, &config, &reply_config, true);
	assert(success);

	if(flipWait)
		co_await _device->flipEvent.async_wait();

	complete();

	co_return;
}
