#include <bragi/helpers-all.hpp>
#include <bragi/helpers-frigg.hpp>
#include <frg/span.hpp>
#include <thor-internal/arch-generic/paging.hpp>
#include <thor-internal/debug.hpp>
#include <thor-internal/fiber.hpp>
#include <thor-internal/main.hpp>
#include <thor-internal/mbus.hpp>
#include <thor-internal/smbios/smbios.hpp>

#include <hw.frigg_bragi.hpp>

namespace {

using namespace thor;

struct NoSmbios final : KernelBusObject {
	coroutine<frg::expected<Error>> handleRequest(LaneHandle lane) override {
		auto [acceptError, conversation] = co_await AcceptSender{lane};
		if (acceptError != Error::success)
			co_return acceptError;

		auto [reqError, reqBuffer] = co_await RecvBufferSender{conversation};
		if (reqError != Error::success)
			co_return reqError;

		auto preamble = bragi::read_preamble(reqBuffer);
		if (preamble.error())
			co_return Error::protocolViolation;

		infoLogger() << "thor: Dismissing conversation due to illegal HW request." << frg::endlog;
		co_await DismissSender{conversation};

		co_return frg::success;
	}

	coroutine<void> run() {
		Properties properties;

		properties.stringProperty(
		    "unix.subsystem", frg::string<KernelAlloc>(*kernelAlloc, "firmware")
		);
		properties.stringProperty(
		    "firmware.type", frg::string<KernelAlloc>(*kernelAlloc, "smbios")
		);
		properties.stringProperty("version", frg::string<KernelAlloc>{*thor::kernelAlloc, "none"});

		auto ret = co_await createObject("smbios-table", std::move(properties));
		assert(ret);
	}
};

struct Smbios3 final : KernelBusObject {
	Smbios3(smbios::Smbios3Header header, frg::vector<uint8_t, KernelAlloc> tableData)
	: header_{header},
	  tableData_{tableData} {}

	static bool validateHeader(smbios::Smbios3Header &header) {
		if (memcmp(&header.anchor, "_SM3_", 5)) {
			infoLogger() << "thor: Invalid SMBIOS3 anchor" << frg::endlog;
			return false;
		}

		uint8_t checksum = 0;
		auto buf = frg::span<uint8_t>(reinterpret_cast<uint8_t *>(&header), sizeof(header));

		for (auto c : buf) {
			checksum += c;
		}

		if (checksum) {
			infoLogger() << "thor: Invalid SMBIOS3 header checksum" << frg::endlog;
			return false;
		}

		return true;
	}

	coroutine<frg::expected<Error>> handleRequest(LaneHandle lane) override {
		auto [acceptError, conversation] = co_await AcceptSender{lane};
		if (acceptError != Error::success)
			co_return acceptError;

		auto [reqError, reqBuffer] = co_await RecvBufferSender{conversation};
		if (reqError != Error::success)
			co_return reqError;

		auto preamble = bragi::read_preamble(reqBuffer);
		if (preamble.error())
			co_return Error::protocolViolation;

		if (preamble.id() == bragi::message_id<managarm::hw::GetSmbiosHeaderRequest>) {
			managarm::hw::GetSmbiosHeaderReply<KernelAlloc> resp{*kernelAlloc};
			resp.set_error(managarm::hw::Errors::SUCCESS);
			resp.set_size(sizeof(header_));

			frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, resp.head_size};
			frg::unique_memory<KernelAlloc> dataBuffer{*kernelAlloc, sizeof(header_)};

			bragi::write_head_only(resp, respBuffer);
			memcpy(dataBuffer.data(), &header_, sizeof(header_));

			auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
			if (respError != Error::success)
				co_return respError;

			auto dataError = co_await SendBufferSender{conversation, std::move(dataBuffer)};
			if (respError != Error::success)
				co_return dataError;
		} else if (preamble.id() == bragi::message_id<managarm::hw::GetSmbiosTableRequest>) {
			managarm::hw::GetSmbiosTableReply<KernelAlloc> resp{*kernelAlloc};
			resp.set_error(managarm::hw::Errors::SUCCESS);
			resp.set_size(tableData_.size());

			frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, resp.head_size};
			frg::unique_memory<KernelAlloc> dataBuffer{*kernelAlloc, tableData_.size()};

			bragi::write_head_only(resp, respBuffer);
			memcpy(dataBuffer.data(), tableData_.data(), tableData_.size());

			auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
			if (respError != Error::success)
				co_return respError;

			auto dataError = co_await SendBufferSender{conversation, std::move(dataBuffer)};
			if (respError != Error::success)
				co_return dataError;
		} else {
			infoLogger() << "thor: Dismissing conversation due to illegal HW request."
			             << frg::endlog;
			co_await DismissSender{conversation};
		}

		co_return frg::success;
	}

	coroutine<void> run() {
		Properties properties;

		properties.stringProperty(
		    "unix.subsystem", frg::string<KernelAlloc>(*kernelAlloc, "firmware")
		);
		properties.stringProperty(
		    "firmware.type", frg::string<KernelAlloc>(*kernelAlloc, "smbios")
		);
		properties.stringProperty("version", frg::string<KernelAlloc>{*thor::kernelAlloc, "3"});

		auto ret = co_await createObject("smbios-table", std::move(properties));
		assert(ret);
	}

private:
	smbios::Smbios3Header header_;
	frg::vector<uint8_t, KernelAlloc> tableData_{*kernelAlloc};
};

constinit frg::manual_box<Smbios3> smbios3;
constinit frg::manual_box<NoSmbios> noSmbios;

} // namespace

namespace thor {

extern ManagarmElfNote<SmbiosData> smbiosNote;
THOR_DEFINE_ELF_NOTE(smbiosNote){elf_note_type::smbiosData, {}};

static initgraph::Task initSmbios3Task{
    &globalInitEngine, "smbios.parse-smbios3", [] {
	    if (!smbiosNote->address)
		    return;

	    PhysicalWindow headerWindow{smbiosNote->address, sizeof(smbios::Smbios3Header)};

	    smbios::Smbios3Header header = {};
	    memcpy(&header, headerWindow.get(), sizeof(header));

	    if (!Smbios3::validateHeader(header))
		    return;

	    size_t tableSize = (header.maxTableSize + (kPageSize - 1)) & ~(kPageSize - 1);
	    PhysicalWindow tableWindow{header.tableAddress, tableSize};

	    frg::vector<uint8_t, KernelAlloc> tableData{*kernelAlloc};
	    tableData.resize(tableSize);
	    memcpy(tableData.data(), tableWindow.get(), tableSize);

	    smbios3.initialize(Smbios3{header, std::move(tableData)});
    }
};

namespace smbios {

void publish() {
	if (smbios3) {
		KernelFiber::run([=] { async::detach_with_allocator(*kernelAlloc, smbios3->run()); });
	} else {
		noSmbios.initialize();
		KernelFiber::run([=] { async::detach_with_allocator(*kernelAlloc, noSmbios->run()); });
	}
}

} // namespace smbios

} // namespace thor
