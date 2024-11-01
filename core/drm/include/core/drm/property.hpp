#pragma once

#include "fwd-decls.hpp"

#include <map>
#include <vector>

#include <helix/memory.hpp>
#include <libdrm/drm.h>

#include "core.hpp"

namespace drm_core {

struct IntPropertyType {};

struct ObjectPropertyType {};

struct BlobPropertyType {};

struct EnumPropertyType {};

using PropertyType =
    std::variant<IntPropertyType, ObjectPropertyType, BlobPropertyType, EnumPropertyType>;

enum PropertyId {
	invalid,
	srcW,
	srcH,
	fbId,
	modeId,
	crtcX,
	crtcY,
	planeType,
	dpms,
	crtcId,
	active,
	srcX,
	srcY,
	crtcW,
	crtcH,
	inFormats,
};

struct Property {
	struct RangeProperty {};
	struct SignedRangeProperty {};
	struct EnumProperty {};
	struct ObjectProperty {};
	struct BlobProperty {};

  private:
	Property(PropertyId id, PropertyType property_type, std::string name)
	    : Property(id, property_type, name, 0) {}

	Property(PropertyId id, PropertyType property_type, std::string name, uint32_t flags)
	    : _id(id),
	      _flags(flags),
	      _propertyType(property_type),
	      _name(name) {
		assert(name.length() < DRM_PROP_NAME_LEN);
	}

  public:
	Property(PropertyId id, BlobProperty, std::string name, uint32_t flags)
	    : Property{id, drm_core::BlobPropertyType{}, name, flags | DRM_MODE_PROP_BLOB} {}

	Property(PropertyId id, ObjectProperty, std::string name, uint32_t flags, uint32_t type)
	    : Property{id, drm_core::ObjectPropertyType{}, name, flags | DRM_MODE_PROP_OBJECT} {
		_type = type;
	}

	Property(PropertyId id, EnumProperty, std::string name, uint32_t flags)
	    : Property{id, drm_core::EnumPropertyType{}, name, flags | DRM_MODE_PROP_ENUM} {}

	Property(PropertyId id, EnumProperty p, std::string name) : Property{id, p, name, 0} {}

	Property(
	    PropertyId id, RangeProperty, std::string name, uint32_t flags, uint32_t min, uint32_t max
	)
	    : Property{id, drm_core::IntPropertyType{}, name, flags | DRM_MODE_PROP_RANGE} {
		_rangeMin = min;
		_rangeMax = max;
	}

	Property(PropertyId id, SignedRangeProperty, std::string name, uint32_t flags)
	    : Property{id, drm_core::IntPropertyType{}, name, flags | DRM_MODE_PROP_SIGNED_RANGE} {}

	Property(
	    PropertyId id,
	    SignedRangeProperty,
	    std::string name,
	    uint32_t flags,
	    int32_t min,
	    int32_t max
	)
	    : Property{id, drm_core::IntPropertyType{}, name, flags | DRM_MODE_PROP_SIGNED_RANGE} {
		_signedRangeMin = min;
		_signedRangeMax = max;
	}

	virtual ~Property() = default;

	virtual bool validate(const Assignment &assignment);

	PropertyId id();
	uint32_t flags();

	uint32_t typeFlags() { return _type; }

	uint64_t rangeMin() { return _rangeMin; }

	uint64_t rangeMax() { return _rangeMax; }

	int64_t signedRangeMin() {
		assert((_flags & DRM_MODE_PROP_EXTENDED_TYPE) == DRM_MODE_PROP_SIGNED_RANGE);
		return _signedRangeMin;
	}

	int64_t signedRangeMax() {
		assert((_flags & DRM_MODE_PROP_EXTENDED_TYPE) == DRM_MODE_PROP_SIGNED_RANGE);
		return _signedRangeMax;
	}

	PropertyType propertyType();
	std::string name();
	void addEnumInfo(uint64_t value, std::string name);
	const std::unordered_map<uint64_t, std::string> &enumInfo();

	/**
	 * Applies an Assignment to a AtomicState.
	 *
	 * In derived classes this method correctly sets the value in the AtomicState.
	 * The default implementation silently drops the Assignment.
	 *
	 * @param assignment
	 * @param state
	 */
	virtual void
	writeToState(const Assignment assignment, std::unique_ptr<drm_core::AtomicState> &state);
	virtual uint32_t intFromState(std::shared_ptr<ModeObject> obj);
	virtual std::shared_ptr<ModeObject> modeObjFromState(std::shared_ptr<ModeObject> obj);

  private:
	PropertyId _id;
	uint32_t _flags;
	uint32_t _type;
	uint64_t _rangeMin = 0;
	uint64_t _rangeMax = UINT32_MAX;
	int64_t _signedRangeMin = INT32_MIN;
	int64_t _signedRangeMax = INT32_MAX;
	PropertyType _propertyType;
	std::string _name;
	std::unordered_map<uint64_t, std::string> _enum_info;
};

/**
 * Holds the changes prepared during a atomic transactions that are to be
 * committed to CRTCs, Planes and Connectors.
 */
struct AtomicState {
	AtomicState(Device *device) : _device(device) {};

	/**
	 * Retrieve the Crtc state from an AtomicState by its Crtc id.
	 *
	 * If the AtomicState does not yet have the requested CrtcState, the currently
	 * active CrtcState is copied over from the correct Crtc. If it already exists,
	 * i.e. has already been modified/touched, it is simply returned.
	 *
	 * @param id The ModeObject id of the Crtc.
	 * @return std::shared_ptr<drm_core::CrtcState> CrtcState for the @p id in the AtomicState
	 */
	std::shared_ptr<CrtcState> crtc(uint32_t id);

	/**
	 * Retrieve the Plane state from an AtomicState by its Plane id.
	 *
	 * If the AtomicState does not yet have the requested PlaneState, the currently
	 * active PlaneState is copied over from the correct Plane. If it already exists,
	 * i.e. has already been modified/touched, it is simply returned.
	 *
	 * @param id The ModeObject id of the Plane.
	 * @return std::shared_ptr<drm_core::PlaneState> PlaneState for the @p id in the AtomicState
	 */
	std::shared_ptr<PlaneState> plane(uint32_t id);

	/**
	 * Retrieve the Connector state from an AtomicState by its Connector id.
	 *
	 * If the AtomicState does not yet have the requested ConnectorState, the currently
	 * active ConnectorState is copied over from the correct Connector. If it already exists,
	 * i.e. has already been modified/touched, it is simply returned.
	 *
	 * @param id The ModeObject id of the Connector.
	 * @return std::shared_ptr<drm_core::ConnectorState> ConnectorState for the @p id in the
	 * AtomicState
	 */
	std::shared_ptr<ConnectorState> connector(uint32_t id);

	std::unordered_map<uint32_t, std::shared_ptr<CrtcState>> &crtc_states(void);
	std::unordered_map<uint32_t, std::shared_ptr<PlaneState>> &plane_states(void);
	std::unordered_map<uint32_t, std::shared_ptr<ConnectorState>> &connector_states(void);

  private:
	Device *_device;

	std::unordered_map<uint32_t, std::shared_ptr<CrtcState>> _crtcStates;
	std::unordered_map<uint32_t, std::shared_ptr<PlaneState>> _planeStates;
	std::unordered_map<uint32_t, std::shared_ptr<ConnectorState>> _connectorStates;
};

struct Assignment {
	/**
	 * Create an Assignment with integer value, be that of int or enum type.
	 *
	 * @param obj ModeObject that this Assignment belongs to.
	 * @param property DRM Property that this Assignment assigns.
	 * @param val Integer value to be set for the @p property of @p obj.
	 * @return drm_core::Assignment Assignment instance to be used for committing the Configuration.
	 */
	static Assignment withInt(std::shared_ptr<ModeObject>, Property *property, uint64_t val);
	static Assignment
	withModeObj(std::shared_ptr<ModeObject>, Property *property, std::shared_ptr<ModeObject>);
	static Assignment
	withBlob(std::shared_ptr<ModeObject>, Property *property, std::shared_ptr<Blob>);

	std::shared_ptr<ModeObject> object;
	Property *property;
	uint64_t intValue;
	std::shared_ptr<ModeObject> objectValue;
	std::shared_ptr<Blob> blobValue;
};

} // namespace drm_core
