#include <thor-internal/fiber.hpp>
#include <thor-internal/main.hpp>
#include <thor-internal/dtb/dtb.hpp>
#include <thor-internal/mbus.hpp>

#include <hw.frigg_bragi.hpp>

#include <bragi/helpers-all.hpp>
#include <bragi/helpers-frigg.hpp>

#ifdef __aarch64__
#include <thor-internal/arch/gic.hpp>
#endif

namespace thor::dt {

struct DtRegister {
	uintptr_t address;
	uintptr_t length;
	uintptr_t offset;
	smarter::shared_ptr<MemoryView> memory;
};

struct DtIrqObject final : IrqObject {
	DtIrqObject(frg::string<KernelAlloc> name, DeviceTreeNode::DeviceIrq deviceIrq, IrqPin *pin)
	: IrqObject{name}, deviceIrq{deviceIrq}, pin{pin} { }

	void dumpHardwareState() override {
		infoLogger() << "thor: DT IRQ " << name() << frg::endlog;
	}

	DeviceTreeNode::DeviceIrq deviceIrq;
	IrqPin *pin;
};

struct MbusNode final : private KernelBusObject {
	MbusNode(DeviceTreeNode *node)
		: node{node}, parent{nullptr}, regs{*kernelAlloc}, irqs{*kernelAlloc} {

		node->associateMbusNode(this);

		if(node->parent())
			parent = node->parent()->getAssociatedMbusNode();

		for(auto &reg : node->reg()) {
			auto offset = reg.addr & (kPageSize - 1);

			regs.emplace_back(
				reg.addr,
				reg.size,
				offset,
				smarter::allocate_shared<HardwareMemory>(*kernelAlloc,
					reg.addr & ~(kPageSize - 1),
					(reg.size + (kPageSize - 1)) & ~(kPageSize - 1),
					CachingMode::mmioNonPosted)
			);
		}
	}

	smarter::shared_ptr<IrqObject> obtainIrqObject(uint32_t index) {
		auto deviceIrq = node->irqs()[index];
#ifdef __aarch64__
		auto *pin = gic->getPin(deviceIrq.id);
#else
#error Missing architecture specific code
#endif

		auto object = smarter::allocate_shared<DtIrqObject>(*kernelAlloc,
				frg::string<KernelAlloc>{*kernelAlloc, "dt-irq."} + node->name(),
				deviceIrq,
				pin);

		IrqPin::attachSink(pin, object.get());

		irqs.push_back(object);
		return object;
	}

	void run(enable_detached_coroutine = {}) {
		Properties properties;

		properties.stringProperty("unix.subsystem", frg::string<KernelAlloc>(*kernelAlloc, "dt"));

		if(parent)
			properties.decStringProperty("drvcore.mbus-parent", parent->mbusId, 1);

		for(auto &compatible : node->compatible()) {
			frg::string<KernelAlloc> prop{*kernelAlloc, "dt.compatible="};
			prop += compatible;
			properties.stringProperty(prop, frg::string<KernelAlloc>{*kernelAlloc, ""});
		}

		auto ret = co_await createObject("dt-node", std::move(properties));
		assert(ret);
		mbusId = ret.value();
	}

	coroutine<frg::expected<Error>> handleRequest(LaneHandle lane) override {
		auto [acceptError, conversation] = co_await AcceptSender{lane};
		if(acceptError != Error::success)
			co_return acceptError;

		auto [reqError, reqBuffer] = co_await RecvBufferSender{conversation};
		if(reqError != Error::success)
			co_return reqError;

		auto preamble = bragi::read_preamble(reqBuffer);
		if(preamble.error())
			co_return Error::protocolViolation;

		auto sendResponse = [] (LaneHandle &conversation,
				managarm::hw::SvrResponse<KernelAlloc> &&resp) -> coroutine<frg::expected<Error>> {
			frg::unique_memory<KernelAlloc> respHeadBuffer{*kernelAlloc,
				resp.head_size};

			frg::unique_memory<KernelAlloc> respTailBuffer{*kernelAlloc,
				resp.size_of_tail()};

			bragi::write_head_tail(resp, respHeadBuffer, respTailBuffer);

			auto respHeadError = co_await SendBufferSender{conversation, std::move(respHeadBuffer)};

			if(respHeadError != Error::success)
				co_return respHeadError;

			auto respTailError = co_await SendBufferSender{conversation, std::move(respTailBuffer)};

			if(respTailError != Error::success)
				co_return respTailError;

			co_return frg::success;
		};

		if(preamble.id() == bragi::message_id<managarm::hw::GetDtInfoRequest>) {
			auto req = bragi::parse_head_only<managarm::hw::GetDtInfoRequest>(reqBuffer, *kernelAlloc);

			if(!req) {
				infoLogger() << "thor: Closing lane due to illegal HW request." << frg::endlog;
				co_return Error::protocolViolation;
			}

			managarm::hw::SvrResponse<KernelAlloc> resp{*kernelAlloc};
			resp.set_error(managarm::hw::Errors::SUCCESS);

			resp.set_num_dt_irqs(node->irqs().size());

			for(const auto &reg : regs) {
				managarm::hw::DtRegister<KernelAlloc> msg(*kernelAlloc);
				msg.set_address(reg.address);
				msg.set_length(reg.length);
				msg.set_offset(reg.offset);
				resp.add_dt_regs(std::move(msg));
			}

			FRG_CO_TRY(co_await sendResponse(conversation, std::move(resp)));
		}else if(preamble.id() == bragi::message_id<managarm::hw::AccessDtRegisterRequest>) {
			auto req = bragi::parse_head_only<managarm::hw::AccessDtRegisterRequest>(reqBuffer, *kernelAlloc);

			if(!req) {
				infoLogger() << "thor: Closing lane due to illegal HW request." << frg::endlog;
				co_return Error::protocolViolation;
			}

			auto index = req->index();

			if(index >= regs.size()) {
				infoLogger() << "thor: Closing lane due to out-ouf-bounds DT register " << index << " in HW request." << frg::endlog;
				co_return Error::illegalArgs;
			}

			MemoryViewDescriptor descriptor{regs[index].memory};

			managarm::hw::SvrResponse<KernelAlloc> resp{*kernelAlloc};
			resp.set_error(managarm::hw::Errors::SUCCESS);

			FRG_CO_TRY(co_await sendResponse(conversation, std::move(resp)));

			auto descError = co_await PushDescriptorSender{conversation, std::move(descriptor)};

			if(descError != Error::success)
				co_return descError;
		}else if(preamble.id() == bragi::message_id<managarm::hw::InstallDtIrqRequest>) {
			auto req = bragi::parse_head_only<managarm::hw::InstallDtIrqRequest>(reqBuffer, *kernelAlloc);

			if(!req) {
				infoLogger() << "thor: Closing lane due to illegal HW request." << frg::endlog;
				co_return Error::protocolViolation;
			}

			auto index = req->index();

			if(index >= node->irqs().size()) {
				infoLogger() << "thor: Closing lane due to out-ouf-bounds DT irq " << index << " in HW request." << frg::endlog;
				co_return Error::illegalArgs;
			}

			auto object = obtainIrqObject(index);

			managarm::hw::SvrResponse<KernelAlloc> resp{*kernelAlloc};
			resp.set_error(managarm::hw::Errors::SUCCESS);

			FRG_CO_TRY(co_await sendResponse(conversation, std::move(resp)));

			auto descError = co_await PushDescriptorSender{conversation, IrqDescriptor{object}};

			if(descError != Error::success)
				co_return descError;
		}else if(preamble.id() == bragi::message_id<managarm::hw::EnableBusIrqRequest>) {
			auto req = bragi::parse_head_only<managarm::hw::EnableBusIrqRequest>(reqBuffer, *kernelAlloc);

			if(!req) {
				infoLogger() << "thor: Closing lane due to illegal HW request." << frg::endlog;
				co_return Error::protocolViolation;
			}

			for(auto &irq : irqs) {
				irq->pin->configure({irq->deviceIrq.trigger, irq->deviceIrq.polarity});
			}

			managarm::hw::SvrResponse<KernelAlloc> resp{*kernelAlloc};
			resp.set_error(managarm::hw::Errors::SUCCESS);

			FRG_CO_TRY(co_await sendResponse(conversation, std::move(resp)));
		}else{
			infoLogger() << "thor: Dismissing conversation due to illegal HW request." << frg::endlog;
			co_await DismissSender{conversation};
		}

		co_return frg::success;
	}

	DeviceTreeNode *node;
	MbusNode *parent;
	frg::vector<DtRegister, KernelAlloc> regs;
	frg::vector<smarter::shared_ptr<DtIrqObject>, KernelAlloc> irqs;
	uint64_t mbusId;
};

frg::manual_box<
	frg::vector<
		smarter::shared_ptr<MbusNode>,
		KernelAlloc
	>
> allNodes;

static initgraph::Task discoverDtNodes{&globalInitEngine, "dt.discover-nodes",
	initgraph::Requires{getDeviceTreeParsedStage()},
	[] {
		allNodes.initialize(*kernelAlloc);

		getDeviceTreeRoot()->forEach([&](DeviceTreeNode *node) -> bool {
			allNodes->emplace_back(smarter::allocate_shared<MbusNode>(*kernelAlloc, node));
			return false;
		});

		infoLogger() << "thor: Found " << allNodes->size() << " DT nodes in total." << frg::endlog;
	}
};

void publishNodes() {
	KernelFiber::run([=] {
		for(auto& node : *allNodes)
			node->run();
	});
}

} // namespace thor::dt
