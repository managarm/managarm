#include <libdrm/drm_fourcc.h>

#include "fs.bragi.hpp"
#include "posix.bragi.hpp"

#include "core/drm/core.hpp"
#include "core/drm/debug.hpp"

namespace drm_core {

static constexpr auto primeFileOperations = protocols::fs::FileOperations{
	.seekAbs = &drm_core::PrimeFile::seekAbs,
	.seekRel = &drm_core::PrimeFile::seekRel,
	.seekEof = &drm_core::PrimeFile::seekEof,
	.accessMemory = &drm_core::PrimeFile::accessMemory,
};

}

async::result<void>
drm_core::File::ioctl(void *object, managarm::fs::CntRequest req,
		helix::UniqueLane conversation) {
	auto self = static_cast<drm_core::File *>(object);
	if(req.command() == DRM_IOCTL_VERSION) {
		helix::SendBuffer send_resp;
		managarm::fs::SvrResponse resp;

		auto version = self->_device->driverVersion();
		auto info = self->_device->driverInfo();

		resp.set_drm_version_major(std::get<0>(version));
		resp.set_drm_version_minor(std::get<1>(version));
		resp.set_drm_version_patchlevel(std::get<2>(version));

		resp.set_drm_driver_name(std::get<0>(info));
		resp.set_drm_driver_desc(std::get<1>(info));
		resp.set_drm_driver_date(std::get<2>(info));

		resp.set_error(managarm::fs::Errors::SUCCESS);

		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
			helix::action(&send_resp, ser.data(), ser.size()));
		co_await transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else if(req.command() == DRM_IOCTL_GET_CAP) {
		helix::SendBuffer send_resp;
		managarm::fs::SvrResponse resp;

		resp.set_error(managarm::fs::Errors::SUCCESS);

		if(req.drm_capability() == DRM_CAP_TIMESTAMP_MONOTONIC) {
			resp.set_drm_value(1);
		}else if(req.drm_capability() == DRM_CAP_DUMB_BUFFER) {
			resp.set_drm_value(1);
		}else if(req.drm_capability() == DRM_CAP_CRTC_IN_VBLANK_EVENT) {
			resp.set_drm_value(1);
		}else if(req.drm_capability() == DRM_CAP_CURSOR_WIDTH) {
			resp.set_drm_value(32);
		}else if(req.drm_capability() == DRM_CAP_CURSOR_HEIGHT) {
			resp.set_drm_value(32);
		}else if(req.drm_capability() == DRM_CAP_PRIME) {
			resp.set_drm_value(DRM_PRIME_CAP_IMPORT | DRM_PRIME_CAP_EXPORT);
		}else{
			std::cout << "core/drm: Unknown capability " << req.drm_capability() << std::endl;
			resp.set_drm_value(0);
			resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
		}

		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
			helix::action(&send_resp, ser.data(), ser.size()));
		co_await transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else if(req.command() == DRM_IOCTL_MODE_GETRESOURCES) {
		helix::SendBuffer send_resp;
		managarm::fs::SvrResponse resp;

		auto &crtcs = self->_device->getCrtcs();
		for(size_t i = 0; i < crtcs.size(); i++) {
			resp.add_drm_crtc_ids(crtcs[i]->id());
		}

		auto &encoders = self->_device->getEncoders();
		for(size_t i = 0; i < encoders.size(); i++) {
			resp.add_drm_encoder_ids(encoders[i]->id());
		}

		auto &connectors = self->_device->getConnectors();
		for(size_t i = 0; i < connectors.size(); i++) {
			resp.add_drm_connector_ids(connectors[i]->id());
		}

		auto &fbs = self->getFrameBuffers();
		for(size_t i = 0; i < fbs.size(); i++) {
			resp.add_drm_fb_ids(fbs[i]->id());
		}

		resp.set_drm_min_width(self->_device->getMinWidth());
		resp.set_drm_max_width(self->_device->getMaxWidth());
		resp.set_drm_min_height(self->_device->getMinHeight());
		resp.set_drm_max_height(self->_device->getMaxHeight());
		resp.set_error(managarm::fs::Errors::SUCCESS);

		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
			helix::action(&send_resp, ser.data(), ser.size()));
		co_await transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else if(req.command() == DRM_IOCTL_MODE_GETCONNECTOR) {
		helix::SendBuffer send_resp;
		helix::SendBuffer send_list;
		managarm::fs::SvrResponse resp;

		auto obj = self->_device->findObject(req.drm_connector_id());
		assert(obj);
		auto conn = obj->asConnector();
		assert(conn);

		auto psbl_enc = conn->getPossibleEncoders();
		for(size_t i = 0; i < psbl_enc.size(); i++) {
			resp.add_drm_encoders(psbl_enc[i]->id());
		}

		resp.set_drm_encoder_id(conn->currentEncoder()->id());
		resp.set_drm_connector_type(conn->connectorType());
		resp.set_drm_connector_type_id(0);
		resp.set_drm_connection(conn->getCurrentStatus()); // DRM_MODE_CONNECTED
		resp.set_drm_mm_width(conn->getPhysicalWidth());
		resp.set_drm_mm_height(conn->getPhysicalHeight());
		resp.set_drm_subpixel(conn->getSubpixel());
		resp.set_drm_num_modes(conn->modeList().size());
		resp.set_error(managarm::fs::Errors::SUCCESS);

		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
			helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
			helix::action(&send_list, conn->modeList().data(),
					std::min(static_cast<size_t>(req.drm_max_modes()), conn->modeList().size())
							* sizeof(drm_mode_modeinfo)));
		co_await transmit.async_wait();
		HEL_CHECK(send_resp.error());
		HEL_CHECK(send_list.error());
	}else if(req.command() == DRM_IOCTL_MODE_GETENCODER) {
		helix::SendBuffer send_resp;
		managarm::fs::SvrResponse resp;

		resp.set_drm_encoder_type(0);

		auto obj = self->_device->findObject(req.drm_encoder_id());
		assert(obj);
		auto enc = obj->asEncoder();
		assert(enc);
		resp.set_drm_crtc_id(enc->currentCrtc()->id());

		uint32_t crtc_mask = 0;
		for(auto crtc : enc->getPossibleCrtcs()) {
			crtc_mask |= 1 << crtc->index;
		}
		resp.set_drm_possible_crtcs(crtc_mask);

		uint32_t clone_mask = 0;
		for(auto clone : enc->getPossibleClones()) {
			clone_mask |= 1 << clone->index;
		}
		resp.set_drm_possible_clones(clone_mask);

		resp.set_error(managarm::fs::Errors::SUCCESS);

		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
			helix::action(&send_resp, ser.data(), ser.size()));
		co_await transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else if(req.command() == DRM_IOCTL_MODE_GETPLANE) {
		helix::SendBuffer send_resp;
		managarm::fs::SvrResponse resp;

		resp.set_drm_encoder_type(0);

		auto obj = self->_device->findObject(req.drm_plane_id());
		assert(obj);
		auto plane = obj->asPlane();
		assert(plane);

		uint32_t crtc_mask = 0;
		for(auto crtc : plane->getPossibleCrtcs()) {
			crtc_mask |= 1 << crtc->index;
		}
		resp.set_drm_possible_crtcs(crtc_mask);

		auto crtc = plane->drmState()->crtc;
		if(crtc != nullptr) {
			resp.set_drm_crtc_id(crtc->id());
		} else {
			resp.set_drm_crtc_id(0);
		}

		auto fb = plane->getFrameBuffer();
		if(fb) {
			resp.set_drm_fb_id(fb->id());
		} else {
			resp.set_drm_fb_id(0);
		}

		resp.set_drm_gamma_size(0);
		resp.add_drm_format_type(DRM_FORMAT_XRGB8888);

		resp.set_error(managarm::fs::Errors::SUCCESS);

		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
			helix::action(&send_resp, ser.data(), ser.size()));
		co_await transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else if(req.command() == DRM_IOCTL_MODE_CREATE_DUMB) {
		helix::SendBuffer send_resp;
		managarm::fs::SvrResponse resp;

		auto pair = self->_device->createDumb(req.drm_width(), req.drm_height(), req.drm_bpp());
		auto handle = self->createHandle(pair.first);
		resp.set_drm_handle(handle);

		resp.set_drm_pitch(pair.second);
		resp.set_drm_size(pair.first->getSize());
		resp.set_error(managarm::fs::Errors::SUCCESS);

		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
			helix::action(&send_resp, ser.data(), ser.size()));
		co_await transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else if(req.command() == DRM_IOCTL_MODE_ADDFB) {
		helix::SendBuffer send_resp;
		managarm::fs::SvrResponse resp;

		if(logDrmRequests)
			std::cout << "core/drm: ADDFB()" << std::endl;

		auto bo = self->resolveHandle(req.drm_handle());
		assert(bo);
		auto buffer = bo->sharedBufferObject();

		auto fourcc = convertLegacyFormat(req.drm_bpp(), req.drm_depth());
		auto fb = self->_device->createFrameBuffer(buffer, req.drm_width(), req.drm_height(),
				fourcc, req.drm_pitch());
		self->attachFrameBuffer(fb);
		resp.set_drm_fb_id(fb->id());
		resp.set_error(managarm::fs::Errors::SUCCESS);

		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
			helix::action(&send_resp, ser.data(), ser.size()));
		co_await transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else if(req.command() == DRM_IOCTL_MODE_RMFB) {
		helix::SendBuffer send_resp;
		managarm::fs::SvrResponse resp;

		auto obj = self->_device->findObject(req.drm_fb_id());
		assert(obj);
		auto fb = obj->asFrameBuffer();
		assert(fb);
		self->detachFrameBuffer(fb);
		resp.set_error(managarm::fs::Errors::SUCCESS);

		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
			helix::action(&send_resp, ser.data(), ser.size()));
		co_await transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else if(req.command() == DRM_IOCTL_MODE_MAP_DUMB) {
		helix::SendBuffer send_resp;
		managarm::fs::SvrResponse resp;

		if(logDrmRequests)
			std::cout << "core/drm: MAP_DUMB(handle " << req.drm_handle() << ")" << std::endl;

		auto bo = self->resolveHandle(req.drm_handle());
		assert(bo);
		auto buffer = bo->sharedBufferObject();

		resp.set_drm_offset(buffer->getMapping());
		resp.set_error(managarm::fs::Errors::SUCCESS);

		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
			helix::action(&send_resp, ser.data(), ser.size()));
		co_await transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else if(req.command() == DRM_IOCTL_MODE_GETCRTC) {
		helix::SendBuffer send_resp;
		helix::SendBuffer send_mode;
		managarm::fs::SvrResponse resp;

		auto obj = self->_device->findObject(req.drm_crtc_id());
		assert(obj);
		auto crtc = obj->asCrtc();
		assert(crtc);

		drm_mode_modeinfo mode_info;
		if(crtc->drmState()->mode) {
			/* TODO: Set x, y, fb_id, gamma_size */
			std::cout << "\e[33mcore/drm: MODE_GETCRTC does not handle x, y or gamma_size\e[39m" << std::endl;
			memcpy(&mode_info, crtc->drmState()->mode->data(), sizeof(drm_mode_modeinfo));
			resp.set_drm_mode_valid(1);
			resp.set_drm_fb_id(crtc->primaryPlane()->drmState()->fb->id());
		}else{
			memset(&mode_info, 0, sizeof(drm_mode_modeinfo));
			resp.set_drm_mode_valid(0);
		}

		resp.set_error(managarm::fs::Errors::SUCCESS);

		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
			helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
			helix::action(&send_mode, &mode_info, sizeof(drm_mode_modeinfo)));
		co_await transmit.async_wait();
		HEL_CHECK(send_resp.error());
		HEL_CHECK(send_mode.error());
	}else if(req.command() == DRM_IOCTL_MODE_SETCRTC) {
		std::vector<char> mode_buffer;
		mode_buffer.resize(sizeof(drm_mode_modeinfo));

		helix::RecvBuffer recv_buffer;
		auto &&buff = helix::submitAsync(conversation, helix::Dispatcher::global(),
				helix::action(&recv_buffer, mode_buffer.data(), sizeof(drm_mode_modeinfo)));
		co_await buff.async_wait();
		HEL_CHECK(recv_buffer.error());

		helix::SendBuffer send_resp;
		managarm::fs::SvrResponse resp;

		auto obj = self->_device->findObject(req.drm_crtc_id());
		assert(obj);
		auto crtc = obj->asCrtc();
		assert(crtc);

		std::vector<drm_core::Assignment> assignments;
		if(req.drm_mode_valid()) {
			auto mode_blob = std::make_shared<Blob>(std::move(mode_buffer));
			auto fb = self->_device->findObject(req.drm_fb_id());
			assert(fb);

			assignments.push_back(Assignment::withBlob(crtc->sharedModeObject(), self->_device->modeIdProperty(), mode_blob));
			assignments.push_back(Assignment::withModeObj(crtc->primaryPlane()->sharedModeObject(), self->_device->fbIdProperty(), fb));
		}else{
			assignments.push_back(Assignment::withBlob(crtc->sharedModeObject(), self->_device->modeIdProperty(), nullptr));
		}

		auto config = self->_device->createConfiguration();
		auto state = self->_device->atomicState();
		auto valid = config->capture(assignments, state);
		assert(valid);
		config->commit(state);

		co_await config->waitForCompletion();

		resp.set_error(managarm::fs::Errors::SUCCESS);

		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
			helix::action(&send_resp, ser.data(), ser.size()));
		co_await transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else if(req.command() == DRM_IOCTL_MODE_PAGE_FLIP) {
		helix::SendBuffer send_resp;
		managarm::fs::SvrResponse resp;

		auto obj = self->_device->findObject(req.drm_crtc_id());
		assert(obj);
		auto crtc = obj->asCrtc();
		assert(crtc);

		std::vector<drm_core::Assignment> assignments;

		auto fb = self->_device->findObject(req.drm_fb_id());
		assert(fb);
		assignments.push_back(Assignment::withModeObj(crtc->primaryPlane()->sharedModeObject(), self->_device->fbIdProperty(), fb));

		auto config = self->_device->createConfiguration();
		auto state = self->_device->atomicState();
		auto valid = config->capture(assignments, state);
		assert(valid);
		config->commit(state);

		self->_retirePageFlip(std::move(config), req.drm_cookie(), crtc->id());

		resp.set_error(managarm::fs::Errors::SUCCESS);

		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
			helix::action(&send_resp, ser.data(), ser.size()));
		co_await transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else if(req.command() == DRM_IOCTL_MODE_DIRTYFB) {
		helix::SendBuffer send_resp;
		managarm::fs::SvrResponse resp;

		resp.set_error(managarm::fs::Errors::SUCCESS);

		auto obj = self->_device->findObject(req.drm_fb_id());
		if(!obj) {
			resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
		} else {
			auto fb = obj->asFrameBuffer();
			assert(fb);
			fb->notifyDirty();
		}

		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
			helix::action(&send_resp, ser.data(), ser.size()));
		co_await transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else if(req.command() == DRM_IOCTL_MODE_CURSOR) {
		helix::SendBuffer send_resp;
		managarm::fs::SvrResponse resp;

		if(logDrmRequests)
			std::cout << "core/drm: MODE_CURSOR()" << std::endl;

		auto crtc_obj = self->_device->findObject(req.drm_crtc_id());
		assert(crtc_obj);
		auto crtc = crtc_obj->asCrtc();

		auto cursor_plane = crtc->cursorPlane();

		if (cursor_plane == nullptr) {
			resp.set_error(managarm::fs::Errors::NO_BACKING_DEVICE);
			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
			helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
			co_return;
		}

		std::vector<Assignment> assignments;
		if (req.drm_flags() == DRM_MODE_CURSOR_BO) {
			resp.set_error(managarm::fs::Errors::SUCCESS);
			auto bo = self->resolveHandle(req.drm_handle());
			auto width = req.drm_width();
			auto height = req.drm_height();

			assignments.push_back(Assignment::withInt(cursor_plane->sharedModeObject(), self->_device->srcWProperty(), width << 16));
			assignments.push_back(Assignment::withInt(cursor_plane->sharedModeObject(), self->_device->srcHProperty(), height << 16));

			if (bo) {
				auto fb = self->_device->createFrameBuffer(bo->sharedBufferObject(), width, height, DRM_FORMAT_ARGB8888, width * 4);
				assert(fb);
				assignments.push_back(Assignment::withModeObj(crtc->cursorPlane()->sharedModeObject(), self->_device->fbIdProperty(), fb));
			} else {
				assignments.push_back(Assignment::withModeObj(crtc->cursorPlane()->sharedModeObject(), self->_device->fbIdProperty(), nullptr));
			}
		}else if (req.drm_flags() == DRM_MODE_CURSOR_MOVE) {
			resp.set_error(managarm::fs::Errors::SUCCESS);
			auto x = req.drm_x();
			auto y = req.drm_y();

			assignments.push_back(Assignment::withInt(cursor_plane->sharedModeObject(), self->_device->crtcXProperty(), x));
			assignments.push_back(Assignment::withInt(cursor_plane->sharedModeObject(), self->_device->crtcYProperty(), y));
		}else{
			printf("\e[35mcore/drm: invalid request whilst handling DRM_IOCTL_MODE_CURSOR\e[39m\n");
			resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
		}

		auto config = self->_device->createConfiguration();
		auto state = self->_device->atomicState();
		auto valid = config->capture(assignments, state);
		assert(valid);
		config->commit(state);

		co_await config->waitForCompletion();

		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
		helix::action(&send_resp, ser.data(), ser.size()));
		co_await transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else if(req.command() == DRM_IOCTL_MODE_DESTROY_DUMB){
		if(logDrmRequests)
			std::cout << "core/drm: DESTROY_DUMB(" << req.drm_handle() << ")" << std::endl;

		self->_buffers.erase(req.drm_handle());
		self->_allocator.free(req.drm_handle());

		helix::SendBuffer send_resp;
		managarm::fs::SvrResponse resp;

		resp.set_error(managarm::fs::Errors::SUCCESS);

		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
			helix::action(&send_resp, ser.data(), ser.size()));
		co_await transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else if(req.command() == DRM_IOCTL_SET_CLIENT_CAP) {
		helix::SendBuffer send_resp;
		managarm::fs::SvrResponse resp;

		if(req.drm_capability() == DRM_CLIENT_CAP_STEREO_3D) {
			std::cout << "\e[31mcore/drm: DRM client cap for stereo 3D unsupported\e[39m" << std::endl;
			resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
		} else if(req.drm_capability() == DRM_CLIENT_CAP_UNIVERSAL_PLANES) {
			self->universalPlanes = true;
			resp.set_error(managarm::fs::Errors::SUCCESS);
		} else if(req.drm_capability() == DRM_CLIENT_CAP_ATOMIC) {
			self->atomic = true;
			self->universalPlanes = true;
			resp.set_error(managarm::fs::Errors::SUCCESS);
		} else {
			std::cout << "\e[31mcore/drm: Attempt to set unknown client capability " << req.drm_capability() << "\e[39m" << std::endl;
			resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
		}

		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
			helix::action(&send_resp, ser.data(), ser.size()));
		co_await transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else if(req.command() == DRM_IOCTL_MODE_OBJ_GETPROPERTIES) {
		helix::SendBuffer send_resp;
		managarm::fs::SvrResponse resp;

		auto obj = self->_device->findObject(req.drm_obj_id());
		assert(obj);

		for(auto ass : obj->getAssignments(self->_device)) {
			resp.add_drm_obj_property_ids(ass.property->id());

			if(std::holds_alternative<IntPropertyType>(ass.property->propertyType())) {
				resp.add_drm_obj_property_values(ass.intValue);
			} else if(std::holds_alternative<EnumPropertyType>(ass.property->propertyType())) {
				resp.add_drm_obj_property_values(ass.intValue);
			} else if(std::holds_alternative<BlobPropertyType>(ass.property->propertyType())) {
				resp.add_drm_obj_property_values(ass.intValue);
			} else if(std::holds_alternative<ObjectPropertyType>(ass.property->propertyType())) {
				if(ass.objectValue) {
					resp.add_drm_obj_property_values(ass.objectValue->id());
				} else {
					resp.add_drm_obj_property_values(0);
				}
			}
		}

		if(!resp.drm_obj_property_ids_size()) {
			std::cout << "\e[31mcore/drm: No properties found for object id " << req.drm_obj_id() << "\e[39m" << std::endl;
		}

		resp.set_error(managarm::fs::Errors::SUCCESS);

		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
			helix::action(&send_resp, ser.data(), ser.size()));
		co_await transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else if(req.command() == DRM_IOCTL_MODE_GETPROPERTY) {
		helix::SendBuffer send_resp;
		managarm::fs::SvrResponse resp;

		uint32_t prop_id = req.drm_property_id();
		auto prop = self->_device->getProperty(prop_id);

		if(prop) {
			resp.set_drm_property_name(prop->name());
			resp.add_drm_property_vals(1);
			resp.set_drm_property_flags(prop->flags());

			if(std::holds_alternative<EnumPropertyType>(prop->propertyType())) {
				for(auto &[value, name] : prop->enumInfo()) {
					resp.add_drm_enum_value(value);
					resp.add_drm_enum_name(name);
				}
			}

			resp.set_error(managarm::fs::Errors::SUCCESS);
		} else {
			resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
		}

		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
			helix::action(&send_resp, ser.data(), ser.size()));
		co_await transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else if(req.command() == DRM_IOCTL_MODE_SETPROPERTY) {
		helix::SendBuffer send_resp;
		managarm::fs::SvrResponse resp;

		std::vector<drm_core::Assignment> assignments;

		auto config = self->_device->createConfiguration();
		auto state = self->_device->atomicState();

		auto mode_obj = self->_device->findObject(req.drm_obj_id());
		assert(mode_obj);

		auto prop = self->_device->getProperty(req.drm_property_id());
		assert(prop);
		auto value = req.drm_property_value();
		auto prop_type = prop->propertyType();


		if(std::holds_alternative<IntPropertyType>(prop_type)) {
			assignments.push_back(Assignment::withInt(mode_obj, prop.get(), value));
		} else if(std::holds_alternative<EnumPropertyType>(prop_type)) {
			assignments.push_back(Assignment::withInt(mode_obj, prop.get(), value));
		} else if(std::holds_alternative<BlobPropertyType>(prop_type)) {
			auto blob = self->_device->findBlob(value);
			assignments.push_back(Assignment::withBlob(mode_obj, prop.get(), blob));
		} else if(std::holds_alternative<ObjectPropertyType>(prop_type)) {
			auto obj = self->_device->findObject(value);
			assignments.push_back(Assignment::withModeObj(mode_obj, prop.get(), obj));
		}

		auto valid = config->capture(assignments, state);
		assert(valid);

		config->commit(state);
		co_await config->waitForCompletion();

		resp.set_error(managarm::fs::Errors::SUCCESS);

		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
			helix::action(&send_resp, ser.data(), ser.size()));
		co_await transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else if(req.command() == DRM_IOCTL_MODE_GETPLANERESOURCES) {
		helix::SendBuffer send_resp;
		managarm::fs::SvrResponse resp;

		auto &crtcs = self->_device->getCrtcs();
		for(size_t i = 0; i < crtcs.size(); i++) {
			resp.add_drm_plane_res(crtcs[i]->primaryPlane()->id());

			if(crtcs[i]->cursorPlane()) {
				resp.add_drm_plane_res(crtcs[i]->cursorPlane()->id());
			}
		}

		resp.set_error(managarm::fs::Errors::SUCCESS);

		if(logDrmRequests)
			std::cout << "core/drm: GETPLANERESOURCES()" << std::endl;

		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
			helix::action(&send_resp, ser.data(), ser.size()));
		co_await transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else if(req.command() == DRM_IOCTL_MODE_GETPROPBLOB) {
		helix::SendBuffer send_resp;
		managarm::fs::SvrResponse resp;

		auto blob = self->_device->findBlob(req.drm_blob_id());

		if(blob) {
			auto data = reinterpret_cast<const uint8_t *>(blob->data());
			for(size_t i = 0; i < blob->size(); i++) {
				resp.add_drm_property_blob(data[i]);
			}

			resp.set_error(managarm::fs::Errors::SUCCESS);
		} else {
			resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
		}

		if(logDrmRequests)
			std::cout << "core/drm: GETPROPBLOB(id " << req.drm_blob_id() << ")" << std::endl;

		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
			helix::action(&send_resp, ser.data(), ser.size()));
		co_await transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else if(req.command() == DRM_IOCTL_MODE_CREATEPROPBLOB) {
		std::vector<char> blob_data;
		blob_data.resize(req.drm_blob_size());

		helix::RecvBuffer recv_buffer;
		auto &&buff = helix::submitAsync(conversation, helix::Dispatcher::global(),
				helix::action(&recv_buffer, blob_data.data(), req.drm_blob_size()));
		co_await buff.async_wait();
		HEL_CHECK(recv_buffer.error());

		helix::SendBuffer send_resp;
		managarm::fs::SvrResponse resp;

		if(!req.drm_blob_size()) {
			resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
		} else {
			auto blob = std::make_shared<Blob>(std::move(blob_data));
			auto id = self->_device->registerBlob(blob);

			resp.set_drm_blob_id(id);
			resp.set_error(managarm::fs::Errors::SUCCESS);
		}

		if(logDrmRequests)
			std::cout << "core/drm: CREATEPROPBLOB() -> id " << resp.drm_blob_id() << std::endl;

		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
			helix::action(&send_resp, ser.data(), ser.size()));
		co_await transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else if(req.command() == DRM_IOCTL_MODE_DESTROYPROPBLOB) {
		helix::SendBuffer send_resp;
		managarm::fs::SvrResponse resp;

		if(!self->_device->deleteBlob(req.drm_blob_id())) {
			resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
		} else {
			resp.set_error(managarm::fs::Errors::SUCCESS);
		}

		if(logDrmRequests)
			std::cout << "core/drm: DESTROYPROPBLOB(id " << req.drm_blob_id() << ")" << std::endl;

		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
			helix::action(&send_resp, ser.data(), ser.size()));
		co_await transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else if(req.command() == DRM_IOCTL_MODE_ATOMIC) {
		helix::SendBuffer send_resp;
		managarm::fs::SvrResponse resp;

		size_t prop_count = 0;
		std::vector<drm_core::Assignment> assignments;

		std::vector<uint32_t> crtc_ids;

		auto config = self->_device->createConfiguration();
		auto state = self->_device->atomicState();

		if(!self->atomic || req.drm_flags() & ~DRM_MODE_ATOMIC_FLAGS || ((req.drm_flags() & DRM_MODE_ATOMIC_TEST_ONLY) && (req.drm_flags() & DRM_MODE_PAGE_FLIP_EVENT))) {
			resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
			goto send;
		}

		for(size_t i = 0; i < req.drm_obj_ids_size(); i++) {
			auto mode_obj = self->_device->findObject(req.drm_obj_ids(i));
			assert(mode_obj);

			if(mode_obj->type() == ObjectType::crtc) {
				crtc_ids.push_back(mode_obj->id());
			}

			for(size_t j = 0; j < req.drm_prop_counts(i); j++) {
				auto prop = self->_device->getProperty(req.drm_props(prop_count + j));
				assert(prop);
				auto value = req.drm_prop_values(prop_count + j);

				auto prop_type = prop->propertyType();

				if(std::holds_alternative<IntPropertyType>(prop_type)) {
					assignments.push_back(Assignment::withInt(mode_obj, prop.get(), value));
				} else if(std::holds_alternative<EnumPropertyType>(prop_type)) {
					assignments.push_back(Assignment::withInt(mode_obj, prop.get(), value));
				} else if(std::holds_alternative<BlobPropertyType>(prop_type)) {
					auto blob = self->_device->findBlob(value);

					assignments.push_back(Assignment::withBlob(mode_obj, prop.get(), blob));
				} else if(std::holds_alternative<ObjectPropertyType>(prop_type)) {
					auto obj = self->_device->findObject(value);

					assignments.push_back(Assignment::withModeObj(mode_obj, prop.get(), obj));
				}
			}

			prop_count += req.drm_prop_counts(i);
		}

		{
			auto valid = config->capture(assignments, state);
			assert(valid);
		}

		if(!(req.drm_flags() & DRM_MODE_ATOMIC_TEST_ONLY)) {
			config->commit(state);
			co_await config->waitForCompletion();
		}

		if(req.drm_flags() & DRM_MODE_PAGE_FLIP_EVENT) {
			assert(crtc_ids.size() == 1);
			self->_retirePageFlip(std::move(config), req.drm_cookie(), crtc_ids.front());
		}

		resp.set_error(managarm::fs::Errors::SUCCESS);

send:
		auto ser = resp.SerializeAsString();
		auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
			helix::action(&send_resp, ser.data(), ser.size()));
		co_await transmit.async_wait();
		HEL_CHECK(send_resp.error());
	}else if(req.command() == DRM_IOCTL_PRIME_HANDLE_TO_FD) {
		managarm::fs::SvrResponse resp;

		// Extract the credentials of the calling thread in order to locate it in POSIX for attaching the file
		auto [proc_creds] = co_await helix_ng::exchangeMsgs(conversation, helix_ng::extractCredentials());
		HEL_CHECK(proc_creds.error());

		auto bo = self->resolveHandle(req.drm_prime_handle());
		assert(bo);
		auto buffer = bo->sharedBufferObject();

		// Create the lane used for serving the PRIME fd
		helix::UniqueLane local_lane, remote_lane;
		std::tie(local_lane, remote_lane) = helix::createStream();
		auto file = smarter::make_shared<drm_core::PrimeFile>(bo->getMemory().first, bo->getSize());

		// Start serving the file
		async::detach(protocols::fs::servePassthrough(
				std::move(local_lane), file, &drm_core::primeFileOperations));

		// Request POSIX to register our file as a passthrough file, while giving out a fd we can pass back to our client
		managarm::posix::CntRequest fd_req;
		fd_req.set_request_type(managarm::posix::CntReqType::FD_SERVE);
		const char *proc_cred_str = proc_creds.credentials();
		for(size_t i = 0; i < 16; i++) {
			fd_req.add_passthrough_credentials(proc_cred_str[i]);
		}

		auto fd_ser = fd_req.SerializeAsString();
		auto [offer, send_req, send_handle, recv_resp] = co_await helix_ng::exchangeMsgs(
			self->_device->_posixLane,
			helix_ng::offer(
				helix_ng::sendBuffer(fd_ser.data(), fd_ser.size()),
				helix_ng::pushDescriptor(helix::BorrowedDescriptor(remote_lane)),
				helix_ng::recvInline())
		);
		HEL_CHECK(offer.error());
		HEL_CHECK(send_req.error());
		HEL_CHECK(send_handle.error());
		HEL_CHECK(recv_resp.error());

		managarm::posix::SvrResponse posix_resp;
		posix_resp.ParseFromArray(recv_resp.data(), recv_resp.length());

		// 'export' the object so that we can locate it from other threads, too
		std::array<char, 16> creds;
		HEL_CHECK(helGetCredentials(remote_lane.getHandle(), 0, creds.data()));

		if(self->exportBufferObject(req.drm_prime_handle(), creds)) {
			resp.set_error(managarm::fs::Errors::SUCCESS);
			resp.set_drm_prime_fd(posix_resp.fd());
		} else {
			resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
		}

		auto ser = resp.SerializeAsString();
		auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBuffer(ser.data(), ser.size())
		);
		HEL_CHECK(send_resp.error());
	}else if(req.command() == DRM_IOCTL_PRIME_FD_TO_HANDLE) {
		managarm::fs::SvrResponse resp;

		// extract the credentials of the land that served the PRIME fd, as this is keying our maps that keep track of it
		auto [creds] = co_await helix_ng::exchangeMsgs(conversation, helix_ng::extractCredentials());
		HEL_CHECK(creds.error());

		// 'import' the BufferObject while returning or creating the DRM handle that references it
		std::array<char, 16> credentials;
		std::copy_n(creds.credentials(), 16, std::begin(credentials));
		auto [bo, handle] = self->importBufferObject(credentials);

		if(bo) {
			resp.set_error(managarm::fs::Errors::SUCCESS);
			resp.set_drm_prime_handle(handle);
		} else {
			resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
		}

		auto ser = resp.SerializeAsString();
		auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBuffer(ser.data(), ser.size())
		);
		HEL_CHECK(send_resp.error());
	}else{
		std::cout << "\e[31m" "core/drm: Unknown ioctl() with ID "
				<< req.command() << "\e[39m" << std::endl;

		auto [dismiss] = co_await helix_ng::exchangeMsgs(
			conversation, helix_ng::dismiss());
		HEL_CHECK(dismiss.error());
	}
}
