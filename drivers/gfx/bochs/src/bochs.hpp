
#include <queue>

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

struct Device {
	void setupCrtc(std::shared_ptr<Crtc>);
	void setupEncoder(std::shared_ptr<Encoder>);
	void attachConnector(std::shared_ptr<Connector>);

	virtual std::unique_ptr<Configuration> createConfiguration() = 0;
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

	std::shared_ptr<Device> _device;
};

struct Configuration {
	virtual bool capture(int width, int height) = 0;
	virtual void dispose() = 0;
	virtual void commit() = 0;
};

struct Crtc {
};

struct Connector {
};

struct Encoder {
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
		GfxDevice * _device;
		int _width;
		int _height;
	};

	GfxDevice(helix::UniqueDescriptor video_ram, void* frame_buffer);
	
	cofiber::no_future initialize();
	std::unique_ptr<drm_backend::Configuration> createConfiguration() override;

public:
	// FIX ME: this is a hack	
	helix::UniqueDescriptor _videoRam;
private:
	arch::io_space _operational;
	void* _frameBuffer;
};

