
#include <queue>
#include <unordered_map>

#include <arch/mem_space.hpp>
#include <async/doorbell.hpp>
#include <async/mutex.hpp>
#include <async/result.hpp>

#include "spec.hpp"

// ----------------------------------------------------------------
// Stuff that belongs in a DRM library.
// ----------------------------------------------------------------
namespace drm_backend {

struct Crtc;
struct Encoder;
struct Connector;
struct Configuration;
struct FrameBuffer;
struct Object;

struct Device {
	virtual std::unique_ptr<Configuration> createConfiguration() = 0;
	
	void setupCrtc(std::shared_ptr<Crtc> crtc);
	void setupEncoder(std::shared_ptr<Encoder> encoder);
	void attachConnector(std::shared_ptr<Connector> connector);
	const std::vector<std::shared_ptr<Crtc>> &getCrtcs();
	const std::vector<std::shared_ptr<Encoder>> &getEncoders();
	const std::vector<std::shared_ptr<Connector>> &getConnectors();
	std::shared_ptr<drm_backend::FrameBuffer> createFrameBuffer();
	
	void registerObject(std::shared_ptr<Object> object);
	Object *findObject(uint32_t);
	
	std::vector<std::shared_ptr<Crtc>> _crtcs;
	std::vector<std::shared_ptr<Encoder>> _encoders;
	std::vector<std::shared_ptr<Connector>> _connectors;
	std::unordered_map<uint32_t, std::shared_ptr<Object>> _objects;
};

struct File {
	File(std::shared_ptr<Device> device)
		:_device(device) { };
	static async::result<int64_t> seek(std::shared_ptr<void> object, int64_t offset);
	static async::result<size_t> read(std::shared_ptr<void> object, void *buffer, size_t length);
	static async::result<void> write(std::shared_ptr<void> object,
			const void *buffer, size_t length);
	static async::result<helix::BorrowedDescriptor> accessMemory(std::shared_ptr<void> object);
	static async::result<void> ioctl(std::shared_ptr<void> object, managarm::fs::CntRequest req,
			helix::UniqueLane conversation);

	void attachFrameBuffer(std::shared_ptr<FrameBuffer> frame_buffer);
	const std::vector<std::shared_ptr<FrameBuffer>> &getFrameBuffers();
	
	std::vector<std::shared_ptr<FrameBuffer>> _frameBuffers;
	std::shared_ptr<Device> _device;
};

struct Configuration {
	virtual bool capture(int width, int height) = 0;
	virtual void dispose() = 0;
	virtual void commit() = 0;
};

struct Crtc {
	virtual Object *asObject() = 0;
};

struct Encoder {
	virtual Object *asObject() = 0;
};

struct Connector {
	virtual const std::vector<Encoder *> &possibleEncoders() = 0;
	virtual Object *asObject() = 0;
};

struct FrameBuffer {
	virtual Object *asObject() = 0;
};

struct Object {
	Object(uint32_t id)
		:_id(id) { };
	
	uint32_t id();
	virtual Encoder *asEncoder();
	virtual Connector *asConnector();
	virtual Crtc *asCrtc();
	virtual FrameBuffer *asFrameBuffer();
	
	uint32_t _id;
};

}

// ----------------------------------------------------------------

struct GfxDevice : drm_backend::Device, std::enable_shared_from_this<GfxDevice> {
	struct Configuration : drm_backend::Configuration {
		Configuration(GfxDevice *device)
		:_device(device) { };
		
		bool capture(int width, int height) override;
		void dispose() override;
		void commit() override;
		
	private:
		GfxDevice *_device;
		int _width;
		int _height;
	};

	struct Connector : drm_backend::Object, drm_backend::Connector {
		Connector(GfxDevice *device);
		
		drm_backend::Connector *asConnector() override;
		drm_backend::Object *asObject() override;
		const std::vector<drm_backend::Encoder *> &possibleEncoders() override;

		std::vector<drm_backend::Encoder *> _encoders;
	};

	struct Encoder : drm_backend::Object, drm_backend::Encoder {
		Encoder(GfxDevice *device);
		
		drm_backend::Encoder *asEncoder() override;
		drm_backend::Object *asObject() override;
	};
	
	struct Crtc : drm_backend::Object, drm_backend::Crtc {
		Crtc(GfxDevice *device);
		
		drm_backend::Crtc *asCrtc() override;
		drm_backend::Object *asObject() override;
	};

	struct FrameBuffer : drm_backend::Object, drm_backend::FrameBuffer {
		FrameBuffer();		

		drm_backend::FrameBuffer *asFrameBuffer() override;
		drm_backend::Object *asObject() override;
	};

	GfxDevice(helix::UniqueDescriptor video_ram, void* frame_buffer);
	
	cofiber::no_future initialize();
	std::unique_ptr<drm_backend::Configuration> createConfiguration() override;

	std::shared_ptr<Crtc> _theCrtc;
	std::shared_ptr<Encoder> _theEncoder;
	std::shared_ptr<Connector> _theConnector;

public:
	// FIX ME: this is a hack	
	helix::UniqueDescriptor _videoRam;
private:
	arch::io_space _operational;
	void* _frameBuffer;
};

