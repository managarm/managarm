#include <bragi/helpers-all.hpp>
#include <bragi/helpers-frigg.hpp>
#include <hw.frigg_bragi.hpp>
#include <thor-internal/acpi/acpi.hpp>
#include <thor-internal/acpi/battery.hpp>
#include <thor-internal/fiber.hpp>
#include <thor-internal/io.hpp>
#include <thor-internal/main.hpp>
#include <uacpi/resources.h>

namespace thor::acpi {

coroutine<void> AcpiObject::run() {
	auto path = uacpi_namespace_node_generate_absolute_path(node);

	Properties acpi_properties;
	acpi_properties.stringProperty("unix.subsystem", frg::string<KernelAlloc>(*kernelAlloc, "acpi"));
	acpi_properties.stringProperty("acpi.path", frg::string<KernelAlloc>(path, *kernelAlloc));
	if(hid_name)
		acpi_properties.stringProperty("acpi.hid", frg::string<KernelAlloc>(*kernelAlloc, hid_name->value));
	if(cid_name && cid_name->num_ids)
		acpi_properties.stringProperty("acpi.cid", frg::string<KernelAlloc>(*kernelAlloc, cid_name->ids[0].value));
	acpi_properties.stringProperty("acpi.instance", frg::to_allocated_string(*kernelAlloc, instance));

	uacpi_free_absolute_path(path);

	mbus_id = (co_await createObject("acpi-object", std::move(acpi_properties))).unwrap();

	completion.raise();
}

coroutine<frg::expected<Error>> AcpiObject::handleRequest(LaneHandle lane) {
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

		if (respHeadError != Error::success)
			co_return respHeadError;

		auto respTailError = co_await SendBufferSender{conversation, std::move(respTailBuffer)};

		if (respTailError != Error::success)
			co_return respTailError;

		co_return frg::success;
	};

	if(preamble.id() == bragi::message_id<managarm::hw::AcpiGetResourcesRequest>) {
		auto req = bragi::parse_head_only<managarm::hw::AcpiGetResourcesRequest>(reqBuffer, *kernelAlloc);

		managarm::hw::AcpiGetResourcesReply<KernelAlloc> resp(*kernelAlloc);

		auto ret = uacpi_for_each_device_resource(node, "_CRS",
		[](void *ctx, uacpi_resource *res){
			auto resp = reinterpret_cast<managarm::hw::AcpiGetResourcesReply<KernelAlloc> *>(ctx);

			switch(res->type) {
				case UACPI_RESOURCE_TYPE_END_TAG:
					break;
				case UACPI_RESOURCE_TYPE_IO:
					for(auto i = res->io.minimum; i <= res->io.maximum; i++) {
						resp->add_io_ports(i);
					}
					break;
				case UACPI_RESOURCE_TYPE_FIXED_IO:
					for(size_t i = 0; i < res->fixed_io.length; i++) {
						resp->add_fixed_io_ports(res->fixed_io.address + i);
					}
					break;
				case UACPI_RESOURCE_TYPE_IRQ:
					for(size_t i = 0; i < res->irq.num_irqs; i++) {
						resp->add_irqs(res->irq.irqs[i]);
					}
					break;
				case UACPI_RESOURCE_TYPE_EXTENDED_IRQ:
					for(size_t i = 0; i < res->extended_irq.num_irqs; i++) {
						resp->add_irqs(res->extended_irq.irqs[i]);
					}
					break;
				default:
					warningLogger() << "thor: unhandled uACPI resource type " << res->type << frg::endlog;
					return UACPI_ITERATION_DECISION_CONTINUE;
			}

			return UACPI_ITERATION_DECISION_CONTINUE;
		}, &resp);

		if(ret == UACPI_STATUS_OK) {
			resp.set_error(managarm::hw::Errors::SUCCESS);
		} else {
			resp.set_error(managarm::hw::Errors::DEVICE_ERROR);
		}

		frg::unique_memory<KernelAlloc> respHeadBuffer{*kernelAlloc, resp.head_size};
		frg::unique_memory<KernelAlloc> respTailBuffer{*kernelAlloc, resp.size_of_tail()};

		bragi::write_head_tail(resp, respHeadBuffer, respTailBuffer);

		auto respHeadError = co_await SendBufferSender{conversation, std::move(respHeadBuffer)};
		if(respHeadError != Error::success)
			co_return respHeadError;

		auto respTailError = co_await SendBufferSender{conversation, std::move(respTailBuffer)};
		if(respTailError != Error::success)
			co_return respTailError;
	}else if(preamble.id() == bragi::message_id<managarm::hw::AccessBarRequest>) {
		auto req = bragi::parse_head_only<managarm::hw::AccessBarRequest>(reqBuffer, *kernelAlloc);

		if (!req) {
			infoLogger() << "thor: Closing lane due to illegal HW request." << frg::endlog;
			co_return Error::protocolViolation;
		}

		managarm::hw::SvrResponse<KernelAlloc> resp{*kernelAlloc};
		auto space = smarter::allocate_shared<IoSpace>(*kernelAlloc);

		struct PortInfo {
			int32_t requested_index;
			int32_t parsed_ports;
			bool success;
			smarter::shared_ptr<IoSpace> space;
		} port_info = {req->index(), 0, false, space};

		// TODO(no92): we should cache this
		auto ret = uacpi_for_each_device_resource(node, "_CRS",
		[](void *ctx, uacpi_resource *res){
			auto info = reinterpret_cast<struct PortInfo *>(ctx);

			switch(res->type) {
				case UACPI_RESOURCE_TYPE_END_TAG:
					break;
				case UACPI_RESOURCE_TYPE_IO:
					if(info->requested_index == info->parsed_ports) {
						for(auto i = res->io.minimum; i <= res->io.maximum; i++) {
							info->space->addPort(i);
							info->success = true;
						}
					}
					info->parsed_ports++;
					break;
				case UACPI_RESOURCE_TYPE_FIXED_IO:
					if(info->requested_index == info->parsed_ports) {
						for(size_t i = 0; i < res->fixed_io.length; i++) {
							info->space->addPort(res->fixed_io.address + i);
							info->success = true;
						}
					}
					info->parsed_ports++;
					break;
				default:
					return UACPI_ITERATION_DECISION_CONTINUE;
			}

			return UACPI_ITERATION_DECISION_CONTINUE;
		}, &port_info);

		if(ret != UACPI_STATUS_OK || !port_info.success) {
			resp.set_error(managarm::hw::Errors::DEVICE_ERROR);
		} else if(port_info.parsed_ports <= port_info.requested_index) {
			resp.set_error(managarm::hw::Errors::OUT_OF_BOUNDS);
		} else {
			resp.set_error(managarm::hw::Errors::SUCCESS);
		}

		FRG_CO_TRY(co_await sendResponse(conversation, std::move(resp)));

		auto ioError = co_await PushDescriptorSender{conversation, IoDescriptor{space}};
		if(ioError != Error::success)
			co_return ioError;
	} else if(preamble.id() == bragi::message_id<managarm::hw::AccessIrqRequest>) {
		auto req = bragi::parse_head_only<managarm::hw::AccessIrqRequest>(reqBuffer, *kernelAlloc);

		if (!req) {
			infoLogger() << "thor: Closing lane due to illegal HW request." << frg::endlog;
			co_return Error::protocolViolation;
		}

		managarm::hw::SvrResponse<KernelAlloc> resp{*kernelAlloc};
		resp.set_error(managarm::hw::Errors::SUCCESS);

		struct InterruptInfo {
			size_t requested_index;
			size_t parsed_irqs;
			std::optional<int> irq;
		} interrupt_info = {req->index(), 0, std::nullopt};

		// TODO(no92): we should cache this
		auto ret = uacpi_for_each_device_resource(node, "_CRS",
		[](void *ctx, uacpi_resource *res){
			auto info = reinterpret_cast<struct InterruptInfo *>(ctx);

			switch(res->type) {
				case UACPI_RESOURCE_TYPE_END_TAG:
					break;
				case UACPI_RESOURCE_TYPE_IRQ:
					for(size_t i = 0; i < res->irq.num_irqs; i++) {
						if(info->parsed_irqs == info->requested_index)
							info->irq = res->irq.irqs[i];
						info->parsed_irqs++;
					}
					break;
				case UACPI_RESOURCE_TYPE_EXTENDED_IRQ:
					for(size_t i = 0; i < res->extended_irq.num_irqs; i++) {
						if(info->parsed_irqs == info->requested_index)
							info->irq = res->extended_irq.irqs[i];
						info->parsed_irqs++;
					}
					break;
				default:
					return UACPI_ITERATION_DECISION_CONTINUE;
			}

			return UACPI_ITERATION_DECISION_CONTINUE;
		}, &interrupt_info);

		auto object = smarter::allocate_shared<GenericIrqObject>(*kernelAlloc,
			frg::string<KernelAlloc>{*kernelAlloc, "isa-irq.ata"});

		if(ret != UACPI_STATUS_OK || !interrupt_info.irq) {
			resp.set_error(managarm::hw::Errors::DEVICE_ERROR);
		} else {
#ifdef __x86_64__
			auto irqOverride = resolveIsaIrq(interrupt_info.irq.value());
			IrqPin::attachSink(getGlobalSystemIrq(irqOverride.gsi), object.get());
#else
			#error "unimplemented"
#endif
		}

		FRG_CO_TRY(co_await sendResponse(conversation, std::move(resp)));

		auto irqError = co_await PushDescriptorSender{conversation, IrqDescriptor{object}};
		if(irqError != Error::success)
			co_return irqError;
	} else {
		infoLogger() << "thor: dismissing conversation due to illegal HW request." << frg::endlog;
		co_await DismissSender{conversation};
	}

	co_return frg::success;
}

initgraph::Stage *getAcpiWorkqueueAvailableStage() {
	static initgraph::Stage s{&globalInitEngine, "acpi.workqueue-available"};
	return &s;
}

static initgraph::Task initAcpiWorkqueueTask{&globalInitEngine, "acpi.init-acpi-workqueue",
	initgraph::Requires{getFibersAvailableStage()},
	initgraph::Entails{getAcpiWorkqueueAvailableStage()},
	[] {
		// Create a fiber to manage requests to the battery mbus objects.
		acpiFiber = KernelFiber::post([] {
			// Do nothing. Our only purpose is to run the associated work queue.
		});

		Scheduler::resume(acpiFiber);
	}
};

} // namespace thor::acpi
