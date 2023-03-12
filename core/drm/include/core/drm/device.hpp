#pragma once

#include <core/id-allocator.hpp>
#include <helix/ipc.hpp>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "fwd-decls.hpp"

#include "mode-object.hpp"

namespace drm_core {

/**
 * This is what gets instantiated as the DRM device, accessible as /dev/dri/card/[num].
 * It represents a complete card that may or may not contain mutiple heads.
 * It holds all the persistent properties that are not bound on a by-fd basis. (?)
 */
struct Device {
	Device();

protected:
	~Device() = default;

public:
	virtual std::unique_ptr<Configuration> createConfiguration() = 0;
	virtual std::pair<std::shared_ptr<BufferObject>, uint32_t> createDumb(uint32_t width,
			uint32_t height, uint32_t bpp) = 0;
	virtual std::shared_ptr<FrameBuffer> createFrameBuffer(std::shared_ptr<BufferObject> buff,
			uint32_t width, uint32_t height, uint32_t format, uint32_t pitch) = 0;
	//returns major, minor, patchlvl
	virtual std::tuple<int, int, int> driverVersion() = 0;
	//returns name, desc, date
	virtual std::tuple<std::string, std::string, std::string> driverInfo() = 0;

	void setupCrtc(Crtc *crtc);
	void setupEncoder(Encoder *encoder);
	void attachConnector(Connector *connector);
	const std::vector<Crtc *> &getCrtcs();
	const std::vector<Encoder *> &getEncoders();
	const std::vector<Connector *> &getConnectors();

	void registerObject(ModeObject *object);
	std::shared_ptr<ModeObject> findObject(uint32_t);

	std::shared_ptr<Blob> registerBlob(std::vector<char> data);
	bool deleteBlob(uint32_t id);
	std::shared_ptr<Blob> findBlob(uint32_t id);

	std::unique_ptr<AtomicState> atomicState();

	uint64_t installMapping(drm_core::BufferObject *bo);

	void setupMinDimensions(uint32_t width, uint32_t height);
	void setupMaxDimensions(uint32_t width, uint32_t height);
	uint32_t getMinWidth();
	uint32_t getMaxWidth();
	uint32_t getMinHeight();
	uint32_t getMaxHeight();

	helix::UniqueDescriptor _posixLane;
private:
	std::vector<Crtc *> _crtcs;
	std::vector<Encoder *> _encoders;
	std::vector<Connector *> _connectors;
	std::unordered_map<uint32_t, ModeObject *> _objects;
	std::unordered_map<uint32_t, std::shared_ptr<Blob>> _blobs;

	id_allocator<uint32_t> _blobIdAllocator;

	/**
	 * Holds (property_id, property) pairs for this device.
	 *
	 * This should not be confused with Assignments, which are attached to ModeObjects and hold
	 * a value. This is only a property, not a property instance!
	 */
	std::unordered_map<uint32_t, std::shared_ptr<drm_core::Property>> _properties;

	id_allocator<uint32_t> _memorySlotAllocator;
	std::map<uint64_t, BufferObject *> _mappings;
	uint32_t _minWidth;
	uint32_t _maxWidth;
	uint32_t _minHeight;
	uint32_t _maxHeight;
	std::shared_ptr<Property> _srcWProperty;
	std::shared_ptr<Property> _srcHProperty;
	std::shared_ptr<Property> _fbIdProperty;
	std::shared_ptr<Property> _modeIdProperty;
	std::shared_ptr<Property> _crtcXProperty;
	std::shared_ptr<Property> _crtcYProperty;
	std::shared_ptr<Property> _planeTypeProperty;
	std::shared_ptr<Property> _dpmsProperty;
	std::shared_ptr<Property> _crtcIdProperty;
	std::shared_ptr<Property> _activeProperty;
	std::shared_ptr<Property> _srcXProperty;
	std::shared_ptr<Property> _srcYProperty;
	std::shared_ptr<Property> _crtcWProperty;
	std::shared_ptr<Property> _crtcHProperty;

	std::map<std::array<char, 16>, std::shared_ptr<drm_core::BufferObject>> _exportedBufferObjects;

public:
	void registerBufferObject(std::shared_ptr<drm_core::BufferObject> obj, std::array<char, 16> creds);
	std::shared_ptr<drm_core::BufferObject> findBufferObject(std::array<char, 16> creds);

	id_allocator<uint32_t> allocator;

	/**
	 * Register a Property @p p with DRM
	 *
	 * Please note that this only makes a Property known to DRM and has no relation
	 * to instances (i.e. values attached to a Property). Assigning values to
	 * Property objects is handled via Configuration, {Crtc,Plane,Connector}States
	 * and Assignments.
	 *
	 * @param p
	 */
	void registerProperty(std::shared_ptr<drm_core::Property> p);

	/**
	 * Obtain a Property object via its @p id.
	 *
	 * @param id
	 * @return std::shared_ptr<drm_core::Property>
	 */
	std::shared_ptr<drm_core::Property> getProperty(uint32_t id);

	Property *srcWProperty();
	Property *srcHProperty();
	Property *fbIdProperty();
	Property *modeIdProperty();
	Property *crtcXProperty();
	Property *crtcYProperty();
	Property *planeTypeProperty();
	Property *dpmsProperty();
	Property *crtcIdProperty();
	Property *activeProperty();
	Property *srcXProperty();
	Property *srcYProperty();
	Property *crtcWProperty();
	Property *crtcHProperty();
};

} //namespace drm_core
