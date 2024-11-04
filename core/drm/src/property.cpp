#include <vector>

#include <helix/memory.hpp>

#include "core/drm/mode-object.hpp"
#include "core/drm/property.hpp"

// ----------------------------------------------------------------
// Property
// ----------------------------------------------------------------

std::vector<drm_core::Assignment> drm_core::ModeObject::getAssignments(std::shared_ptr<Device> dev
) {
	switch (this->type()) {
	case ObjectType::connector: {
		auto connector = this->asConnector();
		assert(connector);
		return connector->getAssignments(dev);
	}
	case ObjectType::crtc: {
		auto crtc = this->asCrtc();
		assert(crtc);
		return crtc->getAssignments(dev);
	}
	case ObjectType::plane: {
		auto plane = this->asPlane();
		assert(plane);
		return plane->getAssignments(dev);
	}
	default: {
		std::cout << "core/drm: ModeObj " << this->id()
		          << " doesn't support querying DRM properties (yet)" << std::endl;
		break;
	}
	}

	std::vector<drm_core::Assignment> assignments = std::vector<drm_core::Assignment>();

	return assignments;
}

bool drm_core::Property::validate(const Assignment &) { return true; }

drm_core::PropertyId drm_core::Property::id() { return _id; }

uint32_t drm_core::Property::flags() { return _flags; }

drm_core::PropertyType drm_core::Property::propertyType() { return _propertyType; }

std::string drm_core::Property::name() { return _name; }

void drm_core::Property::addEnumInfo(uint64_t value, std::string name) {
	assert(std::holds_alternative<EnumPropertyType>(_propertyType));
	_enum_info.insert({value, name});
}

const std::unordered_map<uint64_t, std::string> &drm_core::Property::enumInfo() {
	return _enum_info;
}

void drm_core::Property::writeToState(
    const drm_core::Assignment assignment, std::unique_ptr<drm_core::AtomicState> &state
) {
	(void)assignment;
	(void)state;
	return;
}

uint32_t drm_core::Property::intFromState(std::shared_ptr<drm_core::ModeObject> obj) {
	(void)obj;
	return 0;
}

std::shared_ptr<drm_core::ModeObject>
drm_core::Property::modeObjFromState(std::shared_ptr<drm_core::ModeObject> obj) {
	(void)obj;
	return nullptr;
}

// ----------------------------------------------------------------
// Assignment
// ----------------------------------------------------------------

drm_core::Assignment drm_core::Assignment::withInt(
    std::shared_ptr<drm_core::ModeObject> obj, drm_core::Property *property, uint64_t val
) {
	return drm_core::Assignment{obj, property, val, nullptr, nullptr};
}

drm_core::Assignment drm_core::Assignment::withModeObj(
    std::shared_ptr<drm_core::ModeObject> obj,
    drm_core::Property *property,
    std::shared_ptr<drm_core::ModeObject> modeobj
) {
	return drm_core::Assignment{obj, property, 0, modeobj, nullptr};
}

drm_core::Assignment drm_core::Assignment::withBlob(
    std::shared_ptr<drm_core::ModeObject> obj,
    drm_core::Property *property,
    std::shared_ptr<drm_core::Blob> blob
) {
	return drm_core::Assignment{obj, property, 0, nullptr, blob};
}

std::shared_ptr<drm_core::CrtcState> drm_core::AtomicState::crtc(uint32_t id) {
	if (_crtcStates.contains(id)) {
		return _crtcStates.find(id)->second;
	} else {
		auto crtc = _device->findObject(id)->asCrtc();
		assert(crtc->drmState());
		auto crtc_state = CrtcState(*crtc->drmState());
		auto crtc_state_shared = std::make_shared<drm_core::CrtcState>(crtc_state);
		_crtcStates.insert({id, crtc_state_shared});
		return crtc_state_shared;
	}
}

std::shared_ptr<drm_core::PlaneState> drm_core::AtomicState::plane(uint32_t id) {
	if (_planeStates.contains(id)) {
		return _planeStates.find(id)->second;
	} else {
		auto plane = _device->findObject(id)->asPlane();
		assert(plane->drmState());
		auto plane_state = PlaneState(*plane->drmState());
		auto plane_state_shared = std::make_shared<drm_core::PlaneState>(plane_state);
		_planeStates.insert({id, plane_state_shared});
		return plane_state_shared;
	}
}

std::shared_ptr<drm_core::ConnectorState> drm_core::AtomicState::connector(uint32_t id) {
	if (_connectorStates.contains(id)) {
		return _connectorStates.find(id)->second;
	} else {
		auto connector = _device->findObject(id)->asConnector();
		assert(connector->drmState());
		auto connector_state = ConnectorState(*connector->drmState());
		auto connector_state_shared = std::make_shared<drm_core::ConnectorState>(connector_state);
		_connectorStates.insert({id, connector_state_shared});
		return connector_state_shared;
	}
}

std::unordered_map<uint32_t, std::shared_ptr<drm_core::CrtcState>> &
drm_core::AtomicState::crtc_states(void) {
	return _crtcStates;
}

std::unordered_map<uint32_t, std::shared_ptr<drm_core::PlaneState>> &
drm_core::AtomicState::plane_states(void) {
	return _planeStates;
}

std::unordered_map<uint32_t, std::shared_ptr<drm_core::ConnectorState>> &
drm_core::AtomicState::connector_states(void) {
	return _connectorStates;
}

drm_core::Device::Device() {
	struct SrcWProperty : drm_core::Property {
		SrcWProperty()
		    : drm_core::Property{
		          srcW, RangeProperty{}, "SRC_W", DRM_MODE_PROP_ATOMIC, 0, UINT32_MAX
		      } {}

		bool validate(const Assignment &) override { return true; };

		void
		writeToState(const Assignment assignment, std::unique_ptr<AtomicState> &state) override {
			state->plane(assignment.object->id())->src_w = assignment.intValue >> 16;
		}

		uint32_t intFromState(std::shared_ptr<ModeObject> obj) override {
			auto plane = obj->asPlane();
			assert(plane);
			return plane->drmState()->src_w;
		}
	};
	registerProperty(_srcWProperty = std::make_shared<SrcWProperty>());

	struct SrcHProperty : drm_core::Property {
		SrcHProperty()
		    : drm_core::Property{
		          srcH, RangeProperty{}, "SRC_H", DRM_MODE_PROP_ATOMIC, 0, UINT32_MAX
		      } {}

		bool validate(const Assignment &) override { return true; };

		void
		writeToState(const Assignment assignment, std::unique_ptr<AtomicState> &state) override {
			state->plane(assignment.object->id())->src_h = assignment.intValue >> 16;
		}

		uint32_t intFromState(std::shared_ptr<ModeObject> obj) override {
			auto plane = obj->asPlane();
			assert(plane);
			return plane->drmState()->src_h;
		}
	};
	registerProperty(_srcHProperty = std::make_shared<SrcHProperty>());

	struct FbIdProperty : drm_core::Property {
		FbIdProperty()
		    : drm_core::Property{
		          fbId, ObjectProperty{}, "FB_ID", DRM_MODE_PROP_ATOMIC, DRM_MODE_OBJECT_FB
		      } {}

		bool validate(const Assignment &assignment) override {
			if (!assignment.objectValue)
				return true;

			if (assignment.objectValue->asFrameBuffer())
				return true;

			return false;
		};

		void
		writeToState(const Assignment assignment, std::unique_ptr<AtomicState> &state) override {
			assert(
			    !assignment.objectValue || assignment.objectValue->type() == ObjectType::frameBuffer
			);
			state->plane(assignment.object->id())->fb =
			    static_pointer_cast<FrameBuffer>(assignment.objectValue);
			state->plane(assignment.object->id())
			    ->plane->setCurrentFrameBuffer(
			        static_pointer_cast<FrameBuffer>(assignment.objectValue).get()
			    );
		}

		std::shared_ptr<ModeObject> modeObjFromState(std::shared_ptr<ModeObject> obj) override {
			auto plane = obj->asPlane();
			assert(plane);
			return static_pointer_cast<ModeObject>(plane->drmState()->fb);
		}
	};
	registerProperty(_fbIdProperty = std::make_shared<FbIdProperty>());

	struct ModeIdProperty : drm_core::Property {
		ModeIdProperty()
		    : drm_core::Property{modeId, BlobProperty{}, "MODE_ID", DRM_MODE_PROP_ATOMIC} {}

		bool validate(const Assignment &assignment) override {
			if (!assignment.blobValue) {
				return true;
			}

			if (assignment.blobValue->size() != sizeof(drm_mode_modeinfo))
				return false;

			drm_mode_modeinfo mode_info;
			memcpy(&mode_info, assignment.blobValue->data(), sizeof(drm_mode_modeinfo));
			if (mode_info.hdisplay > mode_info.hsync_start)
				return false;
			if (mode_info.hsync_start > mode_info.hsync_end)
				return false;
			if (mode_info.hsync_end > mode_info.htotal)
				return false;

			if (mode_info.vdisplay > mode_info.vsync_start)
				return false;
			if (mode_info.vsync_start > mode_info.vsync_end)
				return false;
			if (mode_info.vsync_end > mode_info.vtotal)
				return false;

			return true;
		};

		void
		writeToState(const Assignment assignment, std::unique_ptr<AtomicState> &state) override {
			state->crtc(assignment.object->id())->mode = assignment.blobValue;
			state->crtc(assignment.object->id())->modeChanged = true;
		}
	};
	registerProperty(_modeIdProperty = std::make_shared<ModeIdProperty>());

	struct CrtcXProperty : drm_core::Property {
		CrtcXProperty()
		    : drm_core::Property{crtcX, SignedRangeProperty{}, "CRTC_X", DRM_MODE_PROP_ATOMIC} {}

		bool validate(const Assignment &) override { return true; };

		void
		writeToState(const Assignment assignment, std::unique_ptr<AtomicState> &state) override {
			state->plane(assignment.object->id())->crtc_x = assignment.intValue;
		}
	};
	registerProperty(_crtcXProperty = std::make_shared<CrtcXProperty>());

	struct CrtcYProperty : drm_core::Property {
		CrtcYProperty()
		    : drm_core::Property{crtcY, SignedRangeProperty{}, "CRTC_Y", DRM_MODE_PROP_ATOMIC} {}

		bool validate(const Assignment &) override { return true; };

		void
		writeToState(const Assignment assignment, std::unique_ptr<AtomicState> &state) override {
			state->plane(assignment.object->id())->crtc_y = assignment.intValue;
		}
	};
	registerProperty(_crtcYProperty = std::make_shared<CrtcYProperty>());

	struct PlaneTypeProperty : drm_core::Property {
		PlaneTypeProperty()
		    : drm_core::Property{planeType, EnumProperty{}, "type", DRM_MODE_PROP_IMMUTABLE} {
			addEnumInfo(0, "Overlay");
			addEnumInfo(1, "Primary");
			addEnumInfo(2, "Cursor");
		}

		bool validate(const Assignment &assignment) override {
			auto plane = assignment.object->asPlane();
			assert(plane);
			return (static_cast<uint64_t>(plane->type()) == assignment.intValue);
		}
	};
	registerProperty(_planeTypeProperty = std::make_shared<PlaneTypeProperty>());

	struct DpmsProperty : drm_core::Property {
		DpmsProperty() : drm_core::Property{dpms, EnumProperty{}, "DPMS"} {
			addEnumInfo(0, "On");
			addEnumInfo(1, "Standby");
			addEnumInfo(2, "Suspend");
			addEnumInfo(3, "Off");
		}

		bool validate(const Assignment &assignment) override { return (assignment.intValue < 4); }

		void
		writeToState(const Assignment assignment, std::unique_ptr<AtomicState> &state) override {
			state->connector(assignment.object->id())->dpms = assignment.intValue;
		}
	};
	registerProperty(_dpmsProperty = std::make_shared<DpmsProperty>());

	struct CrtcIdProperty : drm_core::Property {
		CrtcIdProperty()
		    : drm_core::Property{
		          crtcId, ObjectProperty{}, "CRTC_ID", DRM_MODE_PROP_ATOMIC, DRM_MODE_OBJECT_CRTC
		      } {}

		bool validate(const Assignment &assignment) override {
			if (assignment.object->type() != ObjectType::connector &&
			    assignment.object->type() != ObjectType::plane) {
				return false;
			}

			if (assignment.objectValue && assignment.objectValue->type() != ObjectType::crtc) {
				return false;
			}

			return true;
		};

		void
		writeToState(const Assignment assignment, std::unique_ptr<AtomicState> &state) override {
			if (assignment.object->type() == ObjectType::connector) {
				state->connector(assignment.object->id())->crtc =
				    static_pointer_cast<Crtc>(assignment.objectValue);
			} else if (assignment.object->type() == ObjectType::plane) {
				state->plane(assignment.object->id())->crtc =
				    static_pointer_cast<Crtc>(assignment.objectValue);
			}
		}
	};
	registerProperty(_crtcIdProperty = std::make_shared<CrtcIdProperty>());

	struct ActiveProperty : drm_core::Property {
		ActiveProperty()
		    : drm_core::Property{active, RangeProperty{}, "ACTIVE", DRM_MODE_PROP_ATOMIC, 0, 1} {}

		bool validate(const Assignment &) override { return true; };

		void
		writeToState(const Assignment assignment, std::unique_ptr<AtomicState> &state) override {
			state->crtc(assignment.object->id())->active = assignment.intValue;
		}
	};
	registerProperty(_activeProperty = std::make_shared<ActiveProperty>());

	struct SrcXProperty : drm_core::Property {
		SrcXProperty()
		    : drm_core::Property{
		          srcX, RangeProperty{}, "SRC_X", DRM_MODE_PROP_ATOMIC, 0, UINT32_MAX
		      } {}

		bool validate(const Assignment &) override { return true; };

		void
		writeToState(const Assignment assignment, std::unique_ptr<AtomicState> &state) override {
			state->plane(assignment.object->id())->src_x = assignment.intValue >> 16;
		}
	};
	registerProperty(_srcXProperty = std::make_shared<SrcXProperty>());

	struct SrcYProperty : drm_core::Property {
		SrcYProperty()
		    : drm_core::Property{
		          srcY, RangeProperty{}, "SRC_Y", DRM_MODE_PROP_ATOMIC, 0, UINT32_MAX
		      } {}

		bool validate(const Assignment &) override { return true; };

		void
		writeToState(const Assignment assignment, std::unique_ptr<AtomicState> &state) override {
			state->plane(assignment.object->id())->src_y = assignment.intValue >> 16;
		}
	};
	registerProperty(_srcYProperty = std::make_shared<SrcYProperty>());

	struct CrtcWProperty : drm_core::Property {
		CrtcWProperty()
		    : drm_core::Property{
		          crtcW, RangeProperty{}, "CRTC_W", DRM_MODE_PROP_ATOMIC, 0, INT32_MAX
		      } {}

		bool validate(const Assignment &) override { return true; };

		void
		writeToState(const Assignment assignment, std::unique_ptr<AtomicState> &state) override {
			state->plane(assignment.object->id())->crtc_w = assignment.intValue;
		}
	};
	registerProperty(_crtcWProperty = std::make_shared<CrtcWProperty>());

	struct CrtcHProperty : drm_core::Property {
		CrtcHProperty()
		    : drm_core::Property{
		          crtcH, RangeProperty{}, "CRTC_H", DRM_MODE_PROP_ATOMIC, 0, INT32_MAX
		      } {}

		bool validate(const Assignment &) override { return true; };

		void
		writeToState(const Assignment assignment, std::unique_ptr<AtomicState> &state) override {
			state->plane(assignment.object->id())->crtc_h = assignment.intValue;
		}
	};
	registerProperty(_crtcHProperty = std::make_shared<CrtcHProperty>());

	struct InFormatsProperty : drm_core::Property {
		InFormatsProperty()
		    : drm_core::Property(
		          inFormats,
		          BlobProperty{},
		          "IN_FORMATS",
		          DRM_MODE_PROP_ATOMIC | DRM_MODE_PROP_IMMUTABLE
		      ) {}

		bool validate(const Assignment &) override { return true; }

		void
		writeToState(const Assignment assignment, std::unique_ptr<AtomicState> &state) override {
			state->plane(assignment.object->id())->in_formats = assignment.blobValue;
		}
	};
	registerProperty(_inFormatsProperty = std::make_shared<InFormatsProperty>());
}
