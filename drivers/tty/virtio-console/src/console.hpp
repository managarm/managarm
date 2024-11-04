#include <memory>
#include <queue>

#include <arch/dma_pool.hpp>
#include <core/virtio/core.hpp>

namespace tty {
namespace virtio_console {

// --------------------------------------------------------
// VirtIO data structures and constants
// --------------------------------------------------------

namespace spec::regs {
inline constexpr arch::scalar_register<uint16_t> cols{0};
inline constexpr arch::scalar_register<uint16_t> rows{2};
inline constexpr arch::scalar_register<uint32_t> maxPorts{4};
inline constexpr arch::scalar_register<uint32_t> emergencyWrite{8};
} // namespace spec::regs

// --------------------------------------------------------
// Device
// --------------------------------------------------------

struct Device {
	Device(std::unique_ptr<virtio_core::Transport> transport);

	async::detached runDevice();

  private:
	arch::contiguous_pool dmaPool_;
	std::unique_ptr<virtio_core::Transport> transport_;
	virtio_core::Queue *rxQueue_;
	virtio_core::Queue *txQueue_;
};

} // namespace virtio_console
} // namespace tty
