#include <libdrm/drm_fourcc.h>

#include <bragi/helpers-std.hpp>
#include "fs.bragi.hpp"
#include <helix/ipc.hpp>
#include "posix.bragi.hpp"

#include "core/drm/core.hpp"
#include "core/drm/debug.hpp"
#include "core/drm/property.hpp"

namespace drm_core {

static constexpr auto primeFileOperations = protocols::fs::FileOperations{
	.seekAbs = &drm_core::PrimeFile::seekAbs,
	.seekRel = &drm_core::PrimeFile::seekRel,
	.seekEof = &drm_core::PrimeFile::seekEof,
	.accessMemory = &drm_core::PrimeFile::accessMemory,
};

}

async::result<void>
drm_core::File::ioctl(void *object, uint32_t id, helix_ng::RecvInlineResult msg,
		helix::UniqueLane conversation) {
	auto self = static_cast<drm_core::File *>(object);

	if(id == managarm::fs::GenericIoctlRequest::message_id) {
		auto req = bragi::parse_head_only<managarm::fs::GenericIoctlRequest>(msg);
		assert(req);

		if(req->command() == DRM_IOCTL_VERSION) {
			helix::SendBuffer send_resp;
			managarm::fs::GenericIoctlReply resp;

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
		}else if(req->command() == DRM_IOCTL_GET_CAP) {
			helix::SendBuffer send_resp;
			managarm::fs::GenericIoctlReply resp;

			if(logDrmRequests)
				std::cout << "core/drm: GET_CAP()" << std::endl;

			resp.set_error(managarm::fs::Errors::SUCCESS);

			if(req->drm_capability() == DRM_CAP_TIMESTAMP_MONOTONIC) {
				resp.set_drm_value(1);
				if(logDrmRequests) std::cout << "\tCAP_TIMESTAMP_MONOTONIC supported" << std::endl;
			}else if(req->drm_capability() == DRM_CAP_DUMB_BUFFER) {
				resp.set_drm_value(1);
				if(logDrmRequests) std::cout << "\tCAP_DUMB_BUFFER supported" << std::endl;
			}else if(req->drm_capability() == DRM_CAP_CRTC_IN_VBLANK_EVENT) {
				resp.set_drm_value(1);
				if(logDrmRequests) std::cout << "\tCAP_CRTC_IN_VBLANK_EVENT supported" << std::endl;
			}else if(req->drm_capability() == DRM_CAP_CURSOR_WIDTH) {
				resp.set_drm_value(32);
				if(logDrmRequests) std::cout << "\tCAP_CURSOR_WIDTH supported" << std::endl;
			}else if(req->drm_capability() == DRM_CAP_CURSOR_HEIGHT) {
				resp.set_drm_value(32);
				if(logDrmRequests) std::cout << "\tCAP_CURSOR_HEIGHT supported" << std::endl;
			}else if(req->drm_capability() == DRM_CAP_PRIME) {
				resp.set_drm_value(DRM_PRIME_CAP_IMPORT | DRM_PRIME_CAP_EXPORT);
				if(logDrmRequests) std::cout << "\tCAP_PRIME supported" << std::endl;
			}else{
				std::cout << "\tUnknown capability " << req->drm_capability() << std::endl;
				resp.set_drm_value(0);
				resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
			}

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
				helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req->command() == DRM_IOCTL_MODE_GETRESOURCES) {
			helix::SendBuffer send_resp;
			managarm::fs::GenericIoctlReply resp;

			if(logDrmRequests)
				std::cout << "core/drm: GETRESOURCES()" << std::endl;

			auto &crtcs = self->_device->getCrtcs();
			for(size_t i = 0; i < crtcs.size(); i++) {
				resp.add_drm_crtc_ids(crtcs[i]->id());
				if(logDrmRequests)
					std::cout << "\tCRTC " << crtcs[i]->id() << std::endl;
			}

			auto &encoders = self->_device->getEncoders();
			for(size_t i = 0; i < encoders.size(); i++) {
				resp.add_drm_encoder_ids(encoders[i]->id());
				if(logDrmRequests)
					std::cout << "\tEncoder " << encoders[i]->id() << std::endl;
			}

			auto &connectors = self->_device->getConnectors();
			for(size_t i = 0; i < connectors.size(); i++) {
				resp.add_drm_connector_ids(connectors[i]->id());
				if(logDrmRequests)
					std::cout << "\tConnector " << connectors[i]->id() << std::endl;
			}

			auto &fbs = self->getFrameBuffers();
			for(size_t i = 0; i < fbs.size(); i++) {
				resp.add_drm_fb_ids(fbs[i]->id());
				if(logDrmRequests)
					std::cout << "\tFB " << fbs[i]->id() << std::endl;
			}

			auto max_width = self->_device->getMaxWidth();
			auto max_height = self->_device->getMaxHeight();

			if(!max_width || !max_height) {
				std::cout << "\e[33mcore/drm: driver-supplied max width/height is empty, defaulting to 16384x16384\e[39m" << std::endl;
				max_width = 16384;
				max_height = 16384;
			}

			resp.set_drm_min_width(self->_device->getMinWidth());
			resp.set_drm_max_width(max_width);
			resp.set_drm_min_height(self->_device->getMinHeight());
			resp.set_drm_max_height(max_height);
			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
				helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req->command() == DRM_IOCTL_MODE_GETCONNECTOR) {
			helix::SendBuffer send_resp;
			helix::SendBuffer send_list;
			managarm::fs::GenericIoctlReply resp;

			if(logDrmRequests)
				std::cout << "core/drm: GETCONNECTOR()" << std::endl;

			auto obj = self->_device->findObject(req->drm_connector_id());
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

			auto assignments = conn->getAssignments(self->_device);

			for(auto ass : assignments) {
				resp.add_drm_obj_property_ids(ass.property->id());

				if(std::holds_alternative<IntPropertyType>(ass.property->propertyType())) {
					resp.add_drm_obj_property_values(ass.intValue);
				} else if(std::holds_alternative<EnumPropertyType>(ass.property->propertyType())) {
					resp.add_drm_obj_property_values(ass.intValue);
				} else if(std::holds_alternative<BlobPropertyType>(ass.property->propertyType())) {
					if(ass.blobValue) {
						resp.add_drm_obj_property_values(ass.blobValue->id());
					} else {
						resp.add_drm_obj_property_values(0);
					}
				} else if(std::holds_alternative<ObjectPropertyType>(ass.property->propertyType())) {
					if(ass.objectValue) {
						resp.add_drm_obj_property_values(ass.objectValue->id());
					} else {
						resp.add_drm_obj_property_values(0);
					}
				}

				if(logDrmRequests) {
					std::cout << "\tproperty " << ass.property->id() << " '" << ass.property->name()
						<< "' = " << resp.drm_obj_property_values(resp.drm_obj_property_values_size() - 1) << std::endl;
				}
			}

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
				helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
				helix::action(&send_list, conn->modeList().data(),
						std::min(static_cast<size_t>(req->drm_max_modes()), conn->modeList().size())
								* sizeof(drm_mode_modeinfo)));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
			HEL_CHECK(send_list.error());
		}else if(req->command() == DRM_IOCTL_MODE_GETENCODER) {
			helix::SendBuffer send_resp;
			managarm::fs::GenericIoctlReply resp;

			if(logDrmRequests)
				std::cout << "core/drm: GETENCODER()" << std::endl;

			resp.set_drm_encoder_type(0);

			auto obj = self->_device->findObject(req->drm_encoder_id());
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
		}else if(req->command() == DRM_IOCTL_MODE_GETPLANE) {
			helix::SendBuffer send_resp;
			managarm::fs::GenericIoctlReply resp;

			if(logDrmRequests)
				std::cout << "core/drm: GETPLANE(" << req->drm_plane_id() << ")" << std::endl;

			resp.set_drm_encoder_type(0);

			auto obj = self->_device->findObject(req->drm_plane_id());
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
				if(logDrmRequests)
					std::cout << "\tCRTC " << crtc->id() << std::endl;
			} else {
				resp.set_drm_crtc_id(0);
			}

			auto fb = plane->getFrameBuffer();
			if(fb != nullptr) {
				resp.set_drm_fb_id(fb->id());
				if(logDrmRequests)
					std::cout << "\tFB " << fb->id() << std::endl;
			} else {
				resp.set_drm_fb_id(0);
			}

			resp.set_drm_gamma_size(0);

			for(auto f : plane->getFormats()) {
				resp.add_drm_format_type(f);
			}

			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
				helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req->command() == DRM_IOCTL_MODE_CREATE_DUMB) {
			helix::SendBuffer send_resp;
			managarm::fs::GenericIoctlReply resp;

			auto pair = self->_device->createDumb(req->drm_width(), req->drm_height(), req->drm_bpp());
			auto handle = self->createHandle(pair.first);
			resp.set_drm_handle(handle);

			resp.set_drm_pitch(pair.second);
			resp.set_drm_size(pair.first->getSize());
			resp.set_error(managarm::fs::Errors::SUCCESS);

			if(logDrmRequests)
				std::cout << "core/drm: CREATE_DUMB(" << req->drm_width() << "x" << req->drm_height() << ") -> <" << resp.drm_handle() << ">" << std::endl;

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
				helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req->command() == DRM_IOCTL_MODE_GETFB2) {
			helix::SendBuffer send_resp;
			managarm::fs::GenericIoctlReply resp;

			if(logDrmRequests)
				std::cout << "core/drm: GETFB2(" << req->drm_fb_id() << ")" << std::endl;

			auto obj = self->_device->findObject(req->drm_fb_id());
			if(!obj || !obj->asFrameBuffer()) {
				resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
			} else {
				auto fb = obj->asFrameBuffer();
				resp.set_drm_width(fb->getWidth());
				resp.set_drm_height(fb->getHeight());
				resp.set_pixel_format(fb->format());
				resp.set_modifier(DRM_FORMAT_MOD_INVALID);
				resp.set_error(managarm::fs::Errors::SUCCESS);
			}

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
				helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req->command() == DRM_IOCTL_MODE_ADDFB) {
			helix::SendBuffer send_resp;
			managarm::fs::GenericIoctlReply resp;

			if(logDrmRequests)
				std::cout << "core/drm: ADDFB(" << req->drm_width() << "x" << req->drm_height() << ", pitch " << req->drm_pitch() << ")";

			auto bo = self->resolveHandle(req->drm_handle());
			assert(bo);
			auto buffer = bo->sharedBufferObject();

			auto fourcc = convertLegacyFormat(req->drm_bpp(), req->drm_depth());
			auto fb = self->_device->createFrameBuffer(buffer, req->drm_width(), req->drm_height(),
					fourcc, req->drm_pitch());
			self->attachFrameBuffer(fb);
			resp.set_drm_fb_id(fb->id());
			resp.set_error(managarm::fs::Errors::SUCCESS);

			if(logDrmRequests)
				std::cout << " -> [" << fb->id() << "]" << std::endl;

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
				helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req->command() == DRM_IOCTL_MODE_ADDFB2) {
			helix::SendBuffer send_resp;
			managarm::fs::GenericIoctlReply resp;

			if(logDrmRequests)
				std::cout << "core/drm: ADDFB2(" << req->drm_width() << "x" << req->drm_height() << ", pitch " << req->drm_pitch() << ")";

			auto bo = self->resolveHandle(req->drm_handle());
			assert(bo);
			auto buffer = bo->sharedBufferObject();

			auto fb = self->_device->createFrameBuffer(buffer, req->drm_width(), req->drm_height(),
					req->drm_fourcc(), req->drm_pitch());
			self->attachFrameBuffer(fb);
			resp.set_drm_fb_id(fb->id());
			resp.set_error(managarm::fs::Errors::SUCCESS);

			if(logDrmRequests)
				std::cout << " -> [" << fb->id() << "]" << std::endl;

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
				helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req->command() == DRM_IOCTL_MODE_RMFB) {
			helix::SendBuffer send_resp;
			managarm::fs::GenericIoctlReply resp;

			if(logDrmRequests)
				std::cout << "core/drm: RMFB([" << req->drm_fb_id() << "])" << std::endl;

			auto obj = self->_device->findObject(req->drm_fb_id());
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
		}else if(req->command() == DRM_IOCTL_MODE_MAP_DUMB) {
			helix::SendBuffer send_resp;
			managarm::fs::GenericIoctlReply resp;

			if(logDrmRequests)
				std::cout << "core/drm: MAP_DUMB(<" << req->drm_handle() << ">)" << std::endl;

			auto bo = self->resolveHandle(req->drm_handle());
			assert(bo);
			auto buffer = bo->sharedBufferObject();

			resp.set_drm_offset(buffer->getMapping());
			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
				helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req->command() == DRM_IOCTL_MODE_GETCRTC) {
			helix::SendBuffer send_resp;
			helix::SendBuffer send_mode;
			managarm::fs::GenericIoctlReply resp;

			if(logDrmRequests)
				std::cout << "core/drm: GETCRTC()" << std::endl;

			auto obj = self->_device->findObject(req->drm_crtc_id());
			assert(obj);
			auto crtc = obj->asCrtc();
			assert(crtc);

			drm_mode_modeinfo mode_info;
			if(crtc->drmState()->mode) {
				memcpy(&mode_info, crtc->drmState()->mode->data(), sizeof(drm_mode_modeinfo));
				resp.set_drm_mode_valid(1);
				resp.set_drm_x(crtc->primaryPlane()->drmState()->src_x);
				resp.set_drm_y(crtc->primaryPlane()->drmState()->src_y);
				/* TODO: wire up gamma once we support that */
				resp.set_drm_gamma_size(0);
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
		}else if(req->command() == DRM_IOCTL_MODE_SETCRTC) {
			std::vector<char> mode_buffer;
			mode_buffer.resize(sizeof(drm_mode_modeinfo));

			if(logDrmRequests)
				std::cout << "core/drm: SETCRTC()" << std::endl;

			helix::RecvBuffer recv_buffer;
			auto &&buff = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&recv_buffer, mode_buffer.data(), sizeof(drm_mode_modeinfo)));
			co_await buff.async_wait();
			HEL_CHECK(recv_buffer.error());

			helix::SendBuffer send_resp;
			managarm::fs::GenericIoctlReply resp;

			auto obj = self->_device->findObject(req->drm_crtc_id());
			assert(obj);
			auto crtc = obj->asCrtc();
			assert(crtc);

			std::vector<drm_core::Assignment> assignments;
			if(req->drm_mode_valid()) {
				auto mode_blob = self->_device->registerBlob(std::move(mode_buffer));
				auto fb = self->_device->findObject(req->drm_fb_id());
				assert(fb);

				assignments.push_back(Assignment::withInt(crtc->sharedModeObject(), self->_device->activeProperty(), true));
				assignments.push_back(Assignment::withBlob(crtc->sharedModeObject(), self->_device->modeIdProperty(), mode_blob));
				assignments.push_back(Assignment::withModeObj(crtc->primaryPlane()->sharedModeObject(), self->_device->fbIdProperty(), fb));
			}else{
				assignments.push_back(Assignment::withInt(crtc->sharedModeObject(), self->_device->activeProperty(), false));
				assignments.push_back(Assignment::withBlob(crtc->sharedModeObject(), self->_device->modeIdProperty(), nullptr));
			}

			auto config = self->_device->createConfiguration();
			auto state = self->_device->atomicState();
			auto valid = config->capture(assignments, state);
			assert(valid);
			config->commit(std::move(state));

			co_await config->waitForCompletion();

			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
				helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req->command() == DRM_IOCTL_MODE_PAGE_FLIP) {
			helix::SendBuffer send_resp;
			managarm::fs::GenericIoctlReply resp;

			if(logDrmRequests)
				std::cout << "core/drm: PAGE_FLIP()" << std::endl;

			auto obj = self->_device->findObject(req->drm_crtc_id());
			assert(obj);
			auto crtc = obj->asCrtc();
			assert(crtc);

			std::vector<drm_core::Assignment> assignments;

			auto fb = self->_device->findObject(req->drm_fb_id());
			assert(fb);
			assignments.push_back(Assignment::withModeObj(crtc->primaryPlane()->sharedModeObject(), self->_device->fbIdProperty(), fb));

			auto config = self->_device->createConfiguration();
			auto state = self->_device->atomicState();
			auto valid = config->capture(assignments, state);
			assert(valid);
			config->commit(std::move(state));

			co_await config->waitForCompletion();
			self->_retirePageFlip(req->drm_cookie(), crtc->id());

			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
				helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req->command() == DRM_IOCTL_MODE_DIRTYFB) {
			helix::SendBuffer send_resp;
			managarm::fs::GenericIoctlReply resp;

			if(logDrmRequests)
				std::cout << "core/drm: DIRTYFB()" << std::endl;

			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto obj = self->_device->findObject(req->drm_fb_id());
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
		}else if(req->command() == DRM_IOCTL_MODE_CURSOR) {
			helix::SendBuffer send_resp;
			managarm::fs::GenericIoctlReply resp;

			if(logDrmRequests)
				std::cout << "core/drm: MODE_CURSOR()" << std::endl;

			auto crtc_obj = self->_device->findObject(req->drm_crtc_id());
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
			if (req->drm_flags() == DRM_MODE_CURSOR_BO) {
				resp.set_error(managarm::fs::Errors::SUCCESS);
				auto bo = self->resolveHandle(req->drm_handle());
				auto width = req->drm_width();
				auto height = req->drm_height();

				assignments.push_back(Assignment::withInt(cursor_plane->sharedModeObject(), self->_device->srcWProperty(), width << 16));
				assignments.push_back(Assignment::withInt(cursor_plane->sharedModeObject(), self->_device->srcHProperty(), height << 16));

				if (bo) {
					auto fb = self->_device->createFrameBuffer(bo->sharedBufferObject(), width, height, DRM_FORMAT_ARGB8888, width * 4);
					assert(fb);
					assignments.push_back(Assignment::withModeObj(crtc->cursorPlane()->sharedModeObject(), self->_device->fbIdProperty(), fb));
				} else {
					assignments.push_back(Assignment::withModeObj(crtc->cursorPlane()->sharedModeObject(), self->_device->fbIdProperty(), nullptr));
				}
			}else if (req->drm_flags() == DRM_MODE_CURSOR_MOVE) {
				resp.set_error(managarm::fs::Errors::SUCCESS);
				auto x = req->drm_x();
				auto y = req->drm_y();

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
			config->commit(std::move(state));

			co_await config->waitForCompletion();

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
			helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req->command() == DRM_IOCTL_MODE_DESTROY_DUMB){
			if(logDrmRequests)
				std::cout << "core/drm: DESTROY_DUMB(" << req->drm_handle() << ")" << std::endl;

			self->_buffers.erase(req->drm_handle());
			self->_allocator.free(req->drm_handle());

			helix::SendBuffer send_resp;
			managarm::fs::GenericIoctlReply resp;

			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
				helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req->command() == DRM_IOCTL_SET_CLIENT_CAP) {
			helix::SendBuffer send_resp;
			managarm::fs::GenericIoctlReply resp;

			if(logDrmRequests)
				std::cout << "core/drm: SET_CLIENT_CAP()" << std::endl;

			if(req->drm_capability() == DRM_CLIENT_CAP_STEREO_3D) {
				std::cout << "\e[31mcore/drm: DRM client cap for stereo 3D unsupported\e[39m" << std::endl;
				resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
			} else if(req->drm_capability() == DRM_CLIENT_CAP_UNIVERSAL_PLANES) {
				self->universalPlanes = true;
				resp.set_error(managarm::fs::Errors::SUCCESS);
			} else if(req->drm_capability() == DRM_CLIENT_CAP_ATOMIC) {
				self->atomic = true;
				self->universalPlanes = true;
				resp.set_error(managarm::fs::Errors::SUCCESS);
			} else {
				std::cout << "\e[31mcore/drm: Attempt to set unknown client capability " << req->drm_capability() << "\e[39m" << std::endl;
				resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
			}

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
				helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req->command() == DRM_IOCTL_MODE_OBJ_GETPROPERTIES) {
			helix::SendBuffer send_resp;
			managarm::fs::GenericIoctlReply resp;

			auto obj = self->_device->findObject(req->drm_obj_id());
			assert(obj);
			resp.set_error(managarm::fs::Errors::SUCCESS);

			if(logDrmRequests)
				std::cout << "core/drm: GETPROPERTIES([" << req->drm_obj_id() << "])" << std::endl;

			for(auto ass : obj->getAssignments(self->_device)) {
				resp.add_drm_obj_property_ids(ass.property->id());

				if(std::holds_alternative<IntPropertyType>(ass.property->propertyType())) {
					resp.add_drm_obj_property_values(ass.intValue);

					if(logDrmRequests)
						std::cout << "\t" << ass.property->name() << " -> int " << ass.intValue << std::endl;
				} else if(std::holds_alternative<EnumPropertyType>(ass.property->propertyType())) {
					resp.add_drm_obj_property_values(ass.intValue);

					if(logDrmRequests) {
						auto enuminfo = ass.property->enumInfo();
						std::string enum_name = "<invalid>";
						if(enuminfo.contains(ass.intValue)) {
							enum_name = enuminfo.at(ass.intValue);
						}
						std::cout << "\t" << ass.property->name() << " -> enum " << enum_name << " (" << ass.intValue << ")" << std::endl;
					}
				} else if(std::holds_alternative<BlobPropertyType>(ass.property->propertyType())) {
					if(ass.blobValue) {
						resp.add_drm_obj_property_values(ass.blobValue->id());
						if(logDrmRequests)
							std::cout << "\t" << ass.property->name() << " -> blob [" << ass.blobValue->id() << "]" << std::endl;
					} else {
						resp.add_drm_obj_property_values(0);
					}
				} else if(std::holds_alternative<ObjectPropertyType>(ass.property->propertyType())) {
					if(ass.objectValue) {
						resp.add_drm_obj_property_values(ass.objectValue->id());
					} else {
						resp.add_drm_obj_property_values(0);
					}
				}
			}

			if(!resp.drm_obj_property_ids_size()) {
				std::cout << "\e[31mcore/drm: No properties found for object [" << req->drm_obj_id() << "]\e[39m" << std::endl;
			}

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
				helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req->command() == DRM_IOCTL_MODE_GETPROPERTY) {
			helix::SendBuffer send_resp;
			managarm::fs::GenericIoctlReply resp;

			uint32_t prop_id = req->drm_property_id();
			auto prop = self->_device->getProperty(prop_id);

			if(logDrmRequests) {
				std::string prop_name = (prop) ? prop->name() : "<invalid>";
				std::cout << "core/drm: GETPROPERTY(" << prop_name << " [" << prop_id << "])" << std::endl;
			}

			if(prop) {
				if(std::holds_alternative<IntPropertyType>(prop->propertyType())) {
					uint32_t type = prop->flags() & (DRM_MODE_PROP_LEGACY_TYPE | DRM_MODE_PROP_EXTENDED_TYPE);
					switch(type) {
						case DRM_MODE_PROP_RANGE: {
							resp.add_drm_property_vals(prop->rangeMin());
							resp.add_drm_property_vals(prop->rangeMax());
							break;
						}
						case DRM_MODE_PROP_SIGNED_RANGE: {
							resp.add_drm_property_vals(prop->signedRangeMin());
							resp.add_drm_property_vals(prop->signedRangeMax());
							break;
						}
						default: {
							std::cout << "core/drm: int property type " << type << " is unhandled by DRM_IOCTL_MODE_GETPROPERTY" << std::endl;
							break;
						}
					}
				} else if(std::holds_alternative<ObjectPropertyType>(prop->propertyType())) {
					resp.add_drm_property_vals(prop->typeFlags());
				} else if(std::holds_alternative<EnumPropertyType>(prop->propertyType())) {
					for(auto &[value, name] : prop->enumInfo()) {
						resp.add_drm_enum_value(value);
						resp.add_drm_enum_name(name);
					}
				}

				resp.set_drm_property_name(prop->name());
				resp.set_drm_property_flags(prop->flags());

				resp.set_error(managarm::fs::Errors::SUCCESS);
			} else {
				resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
			}

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
				helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req->command() == DRM_IOCTL_MODE_SETPROPERTY) {
			helix::SendBuffer send_resp;
			managarm::fs::GenericIoctlReply resp;

			if(logDrmRequests)
				std::cout << "core/drm: SETPROPERTY()" << std::endl;

			std::vector<drm_core::Assignment> assignments;

			auto config = self->_device->createConfiguration();
			auto state = self->_device->atomicState();

			auto mode_obj = self->_device->findObject(req->drm_obj_id());
			assert(mode_obj);

			auto prop = self->_device->getProperty(req->drm_property_id());
			assert(prop);
			auto value = req->drm_property_value();
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

			config->commit(std::move(state));
			co_await config->waitForCompletion();

			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
				helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req->command() == DRM_IOCTL_MODE_GETPLANERESOURCES) {
			helix::SendBuffer send_resp;
			managarm::fs::GenericIoctlReply resp;

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
		}else if(req->command() == DRM_IOCTL_MODE_GETPROPBLOB) {
			helix::SendBuffer send_resp;
			managarm::fs::GenericIoctlReply resp;

			auto blob = self->_device->findBlob(req->drm_blob_id());

			if(logDrmRequests)
				std::cout << "core/drm: GETPROPBLOB([" << req->drm_blob_id() << ((!blob) ? "] [invalid]" : "]") << ")" << std::endl;

			if(blob) {
				auto data = reinterpret_cast<const uint8_t *>(blob->data());
				for(size_t i = 0; i < blob->size(); i++) {
					resp.add_drm_property_blob(data[i]);
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
		}else if(req->command() == DRM_IOCTL_MODE_CREATEPROPBLOB) {
			std::vector<char> blob_data;
			blob_data.resize(req->drm_blob_size());

			helix::RecvBuffer recv_buffer;
			auto &&buff = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&recv_buffer, blob_data.data(), req->drm_blob_size()));
			co_await buff.async_wait();
			HEL_CHECK(recv_buffer.error());

			helix::SendBuffer send_resp;
			managarm::fs::GenericIoctlReply resp;

			if(!req->drm_blob_size()) {
				resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
			} else {
				auto blob = self->_device->registerBlob(std::move(blob_data));

				resp.set_drm_blob_id(blob->id());
				resp.set_error(managarm::fs::Errors::SUCCESS);
			}

			if(logDrmRequests)
				std::cout << "core/drm: CREATEPROPBLOB() -> [" << resp.drm_blob_id() << "]" << std::endl;

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
				helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req->command() == DRM_IOCTL_MODE_DESTROYPROPBLOB) {
			helix::SendBuffer send_resp;
			managarm::fs::GenericIoctlReply resp;

			if(!self->_device->deleteBlob(req->drm_blob_id())) {
				resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
			} else {
				resp.set_error(managarm::fs::Errors::SUCCESS);
			}

			if(logDrmRequests)
				std::cout << "core/drm: DESTROYPROPBLOB([" << req->drm_blob_id() << "])" << std::endl;

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
				helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req->command() == DRM_IOCTL_MODE_ATOMIC) {
			helix::SendBuffer send_resp;
			managarm::fs::GenericIoctlReply resp;

			if(logDrmRequests)
				std::cout << "core/drm: ATOMIC()" << std::endl;

			size_t prop_count = 0;
			std::vector<drm_core::Assignment> assignments;

			std::vector<uint32_t> crtc_ids;

			auto config = self->_device->createConfiguration();
			auto state = self->_device->atomicState();

			if(!self->atomic || req->drm_flags() & ~DRM_MODE_ATOMIC_FLAGS || ((req->drm_flags() & DRM_MODE_ATOMIC_TEST_ONLY) && (req->drm_flags() & DRM_MODE_PAGE_FLIP_EVENT))) {
				resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
				goto send;
			}

			for(size_t i = 0; i < req->drm_obj_ids_size(); i++) {
				auto mode_obj = self->_device->findObject(req->drm_obj_ids(i));
				assert(mode_obj);

				if(logDrmRequests) {
					switch(mode_obj->type()) {
						case ObjectType::crtc:
							std::cout << "\tCRTC (ID " << mode_obj->id() << ")" << std::endl;
							break;
						case ObjectType::connector:
							std::cout << "\tConnector (ID " << mode_obj->id() << ")" << std::endl;
							break;
						case ObjectType::encoder:
							std::cout << "\tEncoder (ID " << mode_obj->id() << ")" << std::endl;
							break;
						case ObjectType::frameBuffer:
							std::cout << "\tFB (ID " << mode_obj->id() << ")" << std::endl;
							break;
						case ObjectType::plane:
							std::cout << "\tPlane (ID " << mode_obj->id() << ")" << std::endl;
							break;
					}
				}

				if(mode_obj->type() == ObjectType::crtc) {
					crtc_ids.push_back(mode_obj->id());
				}

				for(size_t j = 0; j < req->drm_prop_counts(i); j++) {
					auto prop = self->_device->getProperty(req->drm_props(prop_count + j));
					assert(prop);
					auto value = req->drm_prop_values(prop_count + j);

					auto prop_type = prop->propertyType();

					if(std::holds_alternative<IntPropertyType>(prop_type)) {
						assignments.push_back(Assignment::withInt(mode_obj, prop.get(), value));
						if(logDrmRequests)
							std::cout << "\t\t" << prop->name() << " = " << value << " (int)" << std::endl;
					} else if(std::holds_alternative<EnumPropertyType>(prop_type)) {
						assignments.push_back(Assignment::withInt(mode_obj, prop.get(), value));
						if(logDrmRequests)
							std::cout << "\t\t" << prop->name() << " = " << value << " " << prop->enumInfo().at(value) << " (enum)" << std::endl;
					} else if(std::holds_alternative<BlobPropertyType>(prop_type)) {
						auto blob = self->_device->findBlob(value);

						assignments.push_back(Assignment::withBlob(mode_obj, prop.get(), blob));
						if(logDrmRequests)
							std::cout << "\t\t" << prop->name() << " = " << (blob ? std::to_string(blob->id()) : "<none>") << " (blob)" << std::endl;
					} else if(std::holds_alternative<ObjectPropertyType>(prop_type)) {
						auto obj = self->_device->findObject(value);

						assignments.push_back(Assignment::withModeObj(mode_obj, prop.get(), obj));
						if(logDrmRequests)
							std::cout << "\t\t" << prop->name() << " = " << (obj ? std::to_string(obj->id()) : "<none>") << " (modeobject)" << std::endl;
					}
				}

				prop_count += req->drm_prop_counts(i);
			}

			{
				auto valid = config->capture(assignments, state);
				assert(valid);
			}

			if(!(req->drm_flags() & DRM_MODE_ATOMIC_TEST_ONLY)) {
				if(logDrmRequests)
					std::cout << "\tCommitting configuration ..." << std::endl;
				config->commit(std::move(state));
				co_await config->waitForCompletion();
			}

			if(req->drm_flags() & DRM_MODE_PAGE_FLIP_EVENT) {
				co_await config->waitForCompletion();

				for(auto id : crtc_ids) {
					self->_retirePageFlip(req->drm_cookie(), id);
				}
			}

			resp.set_error(managarm::fs::Errors::SUCCESS);

	send:
			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
				helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req->command() == DRM_IOCTL_PRIME_HANDLE_TO_FD) {
			managarm::fs::GenericIoctlReply resp;

			if(logDrmRequests)
				std::cout << "core/drm: PRIME_HANDLE_TO_FD(<" << req->drm_prime_handle() << ">)" << std::endl;

			// Extract the credentials of the calling thread in order to locate it in POSIX for attaching the file
			auto [proc_creds] = co_await helix_ng::exchangeMsgs(conversation, helix_ng::extractCredentials());
			HEL_CHECK(proc_creds.error());

			auto bo = self->resolveHandle(req->drm_prime_handle());
			assert(bo);
			auto buffer = bo->sharedBufferObject();

			// Create the lane used for serving the PRIME fd
			helix::UniqueLane local_lane, remote_lane;
			std::tie(local_lane, remote_lane) = helix::createStream(true);
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
			recv_resp.reset();

			// 'export' the object so that we can locate it from other threads, too
			std::array<char, 16> creds;
			HEL_CHECK(helGetCredentials(remote_lane.getHandle(), 0, creds.data()));

			if(self->exportBufferObject(req->drm_prime_handle(), creds)) {
				resp.set_error(managarm::fs::Errors::SUCCESS);
				resp.set_drm_prime_fd(posix_resp.fd());
				if(logDrmRequests)
					std::cout << "\t-> {" << posix_resp.fd() << "}" << std::endl;
			} else {
				resp.set_error(managarm::fs::Errors::ILLEGAL_ARGUMENT);
			}

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);
			HEL_CHECK(send_resp.error());
		}else if(req->command() == DRM_IOCTL_PRIME_FD_TO_HANDLE) {
			managarm::fs::GenericIoctlReply resp;

			if(logDrmRequests)
				std::cout << "core/drm: PRIME_FD_TO_HANDLE({can't resolve credentials yet})" << std::endl;

			// extract the credentials of the lane that served the PRIME fd, as this is keying our maps that keep track of it
			auto [creds] = co_await helix_ng::exchangeMsgs(conversation, helix_ng::extractCredentials());
			HEL_CHECK(creds.error());

			// 'import' the BufferObject while returning or creating the DRM handle that references it
			std::array<char, 16> credentials;
			std::copy_n(creds.credentials(), 16, std::begin(credentials));
			auto [bo, handle] = self->importBufferObject(credentials);

			if(bo) {
				resp.set_error(managarm::fs::Errors::SUCCESS);
				resp.set_drm_prime_handle(handle);
				if(logDrmRequests)
					std::cout << "\t-> <" << handle << ">" << std::endl;
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
					<< req->command() << "\e[39m" << std::endl;

			auto [dismiss] = co_await helix_ng::exchangeMsgs(
				conversation, helix_ng::dismiss());
			HEL_CHECK(dismiss.error());
		}
	}else if(id == managarm::fs::DrmIoctlGemCloseRequest::message_id) {
		auto req = bragi::parse_head_only<managarm::fs::DrmIoctlGemCloseRequest>(msg);
		assert(req);
		managarm::fs::DrmIoctlGemCloseReply resp;

		if(logDrmRequests)
			std::cout << "core/drm: DRM_IOCTL_GEM_CLOSE(" << req->handle() << ")" << std::endl;

		self->_buffers.erase(req->handle());

		auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
		);
		HEL_CHECK(send_resp.error());
	}else{
		std::cout << "\e[31m" "core/drm: Unknown ioctl() message with ID "
				<< id << "\e[39m" << std::endl;

		auto [dismiss] = co_await helix_ng::exchangeMsgs(
			conversation, helix_ng::dismiss());
		HEL_CHECK(dismiss.error());
	}
}
