
#include <assert.h>
#include <stdio.h>
#include <deque>
#include <experimental/optional>
#include <functional>
#include <iostream>
#include <memory>

#include <arch/dma_pool.hpp>
#include <async/result.hpp>
#include <boost/intrusive/list.hpp>
#include <cofiber.hpp>
#include <frigg/atomic.hpp>
#include <frigg/arch_x86/machine.hpp>
#include <frigg/memory.hpp>
#include <helix/ipc.hpp>
#include <helix/await.hpp>
#include <protocols/hw/client.hpp>
#include <protocols/mbus/client.hpp>
#include <protocols/usb/usb.hpp>
#include <protocols/usb/api.hpp>
#include <protocols/usb/server.hpp>

#include "spec.hpp"
#include "ehci.hpp"

std::vector<std::shared_ptr<Controller>> globalControllers;

// ----------------------------------------------------------------------------
// Memory management.
// ----------------------------------------------------------------------------

namespace {
	arch::contiguous_pool schedulePool;
}

// ----------------------------------------------------------------------------
// Pointer.
// ----------------------------------------------------------------------------

uint32_t physicalPointer(void *ptr) {
	uintptr_t physical;
	HEL_CHECK(helPointerPhysical(ptr, &physical));
	assert((physical & 0xFFFFFFFF) == physical);
	return physical;
}

uint32_t schedulePointer(void *ptr) {
	auto physical = physicalPointer(ptr);
	assert(!(physical & 0x1F));
	return physical;
}

// ----------------------------------------------------------------
// Freestanding PCI discovery functions.
// ----------------------------------------------------------------

COFIBER_ROUTINE(cofiber::no_future, bindController(mbus::Entity entity), ([=] {
	protocols::hw::Device device(COFIBER_AWAIT entity.bind());
	auto info = COFIBER_AWAIT device.getPciInfo();
	assert(info.barInfo[0].ioType == protocols::hw::IoType::kIoTypeMemory);
	auto bar = COFIBER_AWAIT device.accessBar(0);
	auto irq = COFIBER_AWAIT device.accessIrq();
	
	void *actual_pointer;
	HEL_CHECK(helMapMemory(bar.getHandle(), kHelNullHandle, nullptr,
			0, 0x1000, kHelMapReadWrite | kHelMapShareAtFork, &actual_pointer));

	auto controller = std::make_shared<Controller>(actual_pointer, std::move(irq));
	controller->initialize();

	globalControllers.push_back(std::move(controller));
}))

COFIBER_ROUTINE(cofiber::no_future, observeControllers(), ([] {
	auto root = COFIBER_AWAIT mbus::Instance::global().getRoot();

	auto filter = mbus::Conjunction({
		mbus::EqualsFilter("pci-class", "0c"),
		mbus::EqualsFilter("pci-subclass", "03"),
		mbus::EqualsFilter("pci-interface", "20")
	});
	COFIBER_AWAIT root.linkObserver(std::move(filter),
			[] (mbus::AnyEvent event) {
		if(event.type() == typeid(mbus::AttachEvent)) {
			std::cout << "ehci: Detected device" << std::endl;
			bindController(boost::get<mbus::AttachEvent>(event).getEntity());
		}else{
			throw std::runtime_error("Unexpected event type");
		}
	});
}))
	
// ----------------------------------------------------------------
// Controller.
// ----------------------------------------------------------------

Controller::Controller(void *address, helix::UniqueIrq irq)
: _space(address), _irq(frigg::move(irq)) { 
	auto offset = _space.load(cap_regs::caplength);
	_operational = _space.subspace(offset);
	_numPorts = _operational.load(op_regs::hcsparams) & hcsparams::nPorts;
}

void Controller::initialize() {
	// Halt the controller.
	_operational.store(op_regs::usbcmd, usbcmd::run(false));
	while(!(_operational.load(op_regs::usbsts) & usbsts::hcHalted)) {
		// Wait until the controller has stopped.
	}

	// Reset the controller.
	_operational.store(op_regs::usbcmd, usbcmd::hcReset(true) | usbcmd::irqThreshold(0x08));
	while(_operational.load(op_regs::usbcmd) & usbcmd::hcReset) {
		// Wait until the reset is complete.
	}

	// Initialize controller.
	_operational.store(op_regs::usbintr, usbintr::transaction(true) 
			| usbintr::usbError(true) | usbintr::portChange(true) 
			| usbintr::hostError(true));
	_operational.store(op_regs::usbcmd, usbcmd::run(true) | usbcmd::irqThreshold(0x08));
	_operational.store(op_regs::configflag, 0x01);

	pollDevices();
	handleIrqs();
}

COFIBER_ROUTINE(async::result<void>, Controller::pollDevices(), ([=] {
	assert(!(_operational.load(op_regs::hcsparams) & hcsparams::portPower));
	
	while(true) {
		for(int i = 0; i < _numPorts; i++) {
			auto offset = _space.load(cap_regs::caplength);
			auto port_space = _space.subspace(offset + 0x44 + (4 * i));

			if(!(port_space.load(port_regs::sc) & portsc::connectChange))
				continue;
			port_space.store(port_regs::sc, portsc::connectChange(true));
			
			if(!(port_space.load(port_regs::sc) & portsc::connectStatus))
				throw std::runtime_error("EHCI device disconnected");
			
			if((port_space.load(port_regs::sc) & portsc::lineStatus) == 0x01)
				throw std::runtime_error("Device is low-speed");

			port_space.store(port_regs::sc, portsc::portReset(true));	
			// TODO: do not busy-wait.
			uint64_t start;
			HEL_CHECK(helGetClock(&start));
			while(true) {
				uint64_t ticks;
				HEL_CHECK(helGetClock(&ticks));
				if(ticks - start >= 50000000)
					break;
			}
			port_space.store(port_regs::sc, portsc::portReset(false));

			while(port_space.load(port_regs::sc) & portsc::portReset) {

			}
			
			if(!(port_space.load(port_regs::sc) & portsc::portStatus))
				throw std::runtime_error("Device is full-speed");

			std::cout << "High-speed device detected!" << std::endl;

			// TODO: Add directTransfer call
			//COFIBER_AWAIT _directTransfer(0, 0, ControlTransfer{kXferToDevice,
			//		set_address, arch::dma_buffer_view{}}, queue, 8);
			// This queue will become the default control pipe of our new device.
			auto dma_obj = arch::dma_object<QueueHead>{&schedulePool};
			auto queue = new QueueEntity{std::move(dma_obj)};
			_linkAsync(queue);
			
			// Enquire the maximum packet size of the default control pipe.
			arch::dma_object<SetupPacket> get_header{&schedulePool};
			get_header->type = setup_type::targetDevice | setup_type::byStandard
					| setup_type::toHost;
			get_header->request = request_type::getDescriptor;
			get_header->value = descriptor_type::device << 8;
			get_header->index = 0;
			get_header->length = 8;
	
			arch::dma_object<DeviceDescriptor> descriptor{&schedulePool};
			
			COFIBER_AWAIT _directTransfer(0, 0, ControlTransfer{kXferToHost,
					get_header, descriptor.view_buffer().subview(0, 8)}, queue, 8);

			std::cout << "descriptor: " << std::endl;
			std::cout << "    deviceClass: " << (int)(descriptor->deviceClass) << std::endl;
			std::cout << "    deviceSubclass: " << (int)(descriptor->deviceSubclass) << std::endl;
			std::cout << "    deviceProtocol: " << (int)(descriptor->deviceProtocol) << std::endl;
			std::cout << "    maxPacketSize: " << (int)(descriptor->maxPacketSize) << std::endl;
		}

		COFIBER_AWAIT _pollDoorbell.async_wait();
	}
}))

COFIBER_ROUTINE(cofiber::no_future, Controller::handleIrqs(), ([=] {
	while(true) {
		helix::AwaitIrq await_irq;
		auto &&submit = helix::submitAwaitIrq(_irq, &await_irq, helix::Dispatcher::global());
		COFIBER_AWAIT submit.async_wait();
		HEL_CHECK(await_irq.error());

		// _updateFrame();

		auto status = _operational.load(op_regs::usbsts);
		assert(!(status & usbsts::hostError));
		if(!(status & usbsts::transactionIrq) && !(status & usbsts::errorIrq)
			 	&& !(status & usbsts::portChange))
			continue;

		if(status & usbsts::errorIrq)
			printf("ehci: Error interrupt\n");
		_operational.store(op_regs::usbsts, usbsts::transactionIrq(true) | usbsts::errorIrq(true)
			| usbsts::portChange(true));
		
		printf("ehci: Processing transfers.\n");
		_progressSchedule();
	}
}))

// ------------------------------------------------------------------------
// Schedule classes.
// ------------------------------------------------------------------------

Controller::QueueEntity::QueueEntity(arch::dma_object<QueueHead> the_head)
: head(std::move(the_head)) {
	head->horizontalPtr.store(qh_horizontal::terminate(false)
			| qh_horizontal::typeSelect(0x01) 
			| qh_horizontal::horizontalPtr(schedulePointer(head.data())));
	head->flags.store(qh_flags::reclaimHead(true)
			| qh_flags::deviceAddr(0x00)
			| qh_flags::endpointSpeed(0x02)
			| qh_flags::endpointNumber(0)
			| qh_flags::maxPacketLength(64)
//			| qh_flags::dataToggleCtrl(true)
			);
	head->mask.store(qh_mask::interruptScheduleMask(0x00)
			| qh_mask::multiplier(0x01));
	head->curTd.store(qh_curTd::curTd(0x00));
	head->nextTd.store(qh_nextTd::terminate(true));
	head->altTd.store(qh_altTd::terminate(true));
	head->status.store(qh_status::active(false));
	head->bufferPtr0.store(qh_buffer::bufferPtr(0x00));
	head->bufferPtr1.store(qh_buffer::bufferPtr(0x00));
	head->bufferPtr2.store(qh_buffer::bufferPtr(0x00));
	head->bufferPtr3.store(qh_buffer::bufferPtr(0x00));
	head->bufferPtr4.store(qh_buffer::bufferPtr(0x00));
}
	
// ------------------------------------------------------------------------
// Transfer functions.
// ------------------------------------------------------------------------

auto Controller::_buildControl(int address, int pipe, XferFlags dir,
		arch::dma_object_view<SetupPacket> setup, arch::dma_buffer_view buffer,
		size_t max_packet_size) -> Transaction * {
	assert((dir == kXferToDevice) || (dir == kXferToHost));

	arch::dma_array<TransferDescriptor> transfers{&schedulePool, 3};

	transfers[0].nextTd.store(td_ptr::ptr(physicalPointer(&transfers[1]))
			| td_ptr::terminate(false));
	transfers[0].altTd.store(td_ptr::terminate(true));
	transfers[0].status.store(td_status::active(true) | td_status::pidCode(0x02)
			| td_status::interruptOnComplete(true) 
			| td_status::totalBytes(sizeof(SetupPacket))
			| td_status::dataToggle(false));
	transfers[0].bufferPtr0.store(td_buffer::bufferPtr(physicalPointer(setup.data())));
	transfers[0].bufferPtr1.store(td_buffer::curOffset(0));
	transfers[0].bufferPtr2.store(td_buffer::curOffset(0));
	transfers[0].bufferPtr3.store(td_buffer::curOffset(0));
	transfers[0].bufferPtr4.store(td_buffer::curOffset(0));

	transfers[1].nextTd.store(td_ptr::ptr(physicalPointer(&transfers[2]))
			| td_ptr::terminate(false));
	transfers[1].altTd.store(td_ptr::terminate(true));
	transfers[1].status.store(td_status::active(true)
			| td_status::pidCode(dir == 0 ? 0x00 : 0x01)
			| td_status::interruptOnComplete(true)
			| td_status::totalBytes(buffer.size()));
	transfers[1].bufferPtr0.store(td_buffer::bufferPtr(physicalPointer(buffer.data())));
	transfers[1].bufferPtr1.store(td_buffer::curOffset(0));
	transfers[1].bufferPtr2.store(td_buffer::curOffset(0));
	transfers[1].bufferPtr3.store(td_buffer::curOffset(0));
	transfers[1].bufferPtr4.store(td_buffer::curOffset(0));

	transfers[2].nextTd.store(td_ptr::terminate(true));
	transfers[2].altTd.store(td_ptr::terminate(true));
	transfers[2].status.store(td_status::active(true) 
			| td_status::interruptOnComplete(true));
	transfers[2].bufferPtr0.store(td_buffer::curOffset(0));
	transfers[2].bufferPtr1.store(td_buffer::curOffset(0));
	transfers[2].bufferPtr2.store(td_buffer::curOffset(0));
	transfers[2].bufferPtr3.store(td_buffer::curOffset(0));
	transfers[2].bufferPtr4.store(td_buffer::curOffset(0));

	return new Transaction{std::move(transfers)};
}

async::result<void> Controller::_directTransfer(int address, int pipe, ControlTransfer info,
		QueueEntity *queue, size_t max_packet_size) {
	auto transaction = _buildControl(address, pipe, info.flags,
			info.setup, info.buffer, max_packet_size);
	_linkTransaction(queue, transaction);
	return transaction->promise.async_get();
}

// ------------------------------------------------------------------------
// Schedule management.
// ------------------------------------------------------------------------

void Controller::_linkAsync(QueueEntity *entity) {
	if(_asyncSchedule.empty()) {
		_operational.store(op_regs::asynclistaddr, schedulePointer(entity->head.data()));
		_operational.store(op_regs::usbcmd, usbcmd::asyncEnable(true) 
				| usbcmd::run(true) | usbcmd::irqThreshold(0x08));
	}else{
		_asyncSchedule.back().head->horizontalPtr.store(
				qh_horizontal::horizontalPtr(schedulePointer(entity->head.data())));
		_asyncSchedule.back().head->nextTd.store(
				qh_nextTd::nextTd(schedulePointer(entity->head.data())));
	}
	_asyncSchedule.push_back(*entity);
}

void Controller::_linkTransaction(QueueEntity *queue, Transaction *transaction) {
	if(queue->transactions.empty()) {
		queue->head->nextTd.store(qh_nextTd::nextTd(
				schedulePointer(&transaction->transfers[0]))); 
		queue->head->altTd.store(qh_nextTd::nextTd(
				schedulePointer(&transaction->transfers[0])));
		asm volatile ("" : : : "memory");
	}

	queue->transactions.push_back(*transaction);
}

void Controller::_progressSchedule() {
	auto it = _asyncSchedule.begin();
	while(it != _asyncSchedule.end()) {
		_progressQueue(&(*it));
		++it;
	}
}

void Controller::_progressQueue(QueueEntity *entity) {
	if(entity->transactions.empty())
		return;

	auto active = &entity->transactions.front();
	while(active->numComplete < active->transfers.size()) {
		auto &transfer = active->transfers[active->numComplete];
		if((transfer.status.load() & td_status::active)
				|| (transfer.status.load() & td_status::transactionError)
				|| (transfer.status.load() & td_status::babbleDetected)
				|| (transfer.status.load() & td_status::halted)
				|| (transfer.status.load() & td_status::dataBufferError))
			break;

		active->numComplete++;
	}
	
	if(active->numComplete == active->transfers.size()) {
		printf("Transfer complete!\n");
		active->promise.set_value();

		// Clean up the Queue.
		entity->transactions.pop_front();
		// TODO: _reclaim(active);
		
		// Schedule the next transaction.
		assert(entity->head->nextTd.load() & td_ptr::terminate);

		if(!entity->transactions.empty()) {
			auto front = &entity->transactions.front();
			entity->head->nextTd.store(qh_nextTd::nextTd(schedulePointer(&front->transfers[0])));
		}
	}else if((active->transfers[active->numComplete].status.load() & td_status::transactionError)
			|| (active->transfers[active->numComplete].status.load() & td_status::babbleDetected)
			|| (active->transfers[active->numComplete].status.load() & td_status::halted)
			|| (active->transfers[active->numComplete].status.load() & td_status::dataBufferError)) {
		printf("Transfer error!\n");
		
		_dump(entity);
		
		// Clean up the Queue.
		entity->transactions.pop_front();
		// TODO: _reclaim(active);
	}
	
}

// ----------------------------------------------------------------------------
// Debugging functions.
// ----------------------------------------------------------------------------

void Controller::_dump(QueueEntity *entity) {
	std::cout << "queue_head_status: " << std::endl;
	std::cout << "    pingError: " << (int)(entity->head->status.load() 
			& qh_status::pingError) << std::endl;
	std::cout << "    splitXState: " << (int)(entity->head->status.load() 
			& qh_status::splitXState) << std::endl;
	std::cout << "    missedFrame: " << (int)(entity->head->status.load() 
			& qh_status::missedFrame) << std::endl;
	std::cout << "    transactionError: " << (int)(entity->head->status.load() 
			& qh_status::transactionError) << std::endl;
	std::cout << "    babbleDetected: " << (int)(entity->head->status.load()
			& qh_status::babbleDetected) << std::endl;
	std::cout << "    dataBufferError: " << (int)(entity->head->status.load()
			& qh_status::dataBufferError) << std::endl;
	std::cout << "    halted: " << (int)(entity->head->status.load()
			& qh_status::halted) << std::endl;
	std::cout << "    pidCode: " << (int)(entity->head->status.load()
			& qh_status::pidCode) << std::endl;
	std::cout << "    errorCounter: " << (int)(entity->head->status.load()
			& qh_status::errorCounter) << std::endl;
	std::cout << "    cPage: " << (int)(entity->head->status.load()
			& qh_status::cPage) << std::endl;
	std::cout << "    interruptOnComplete: " << (int)(entity->head->status.load()
			& qh_status::interruptOnComplete) << std::endl;
	std::cout << "    totalBytes: " << (int)(entity->head->status.load()
			& qh_status::totalBytes) << std::endl;
	std::cout << "    dataToggle: " << (int)(entity->head->status.load()
			& qh_status::dataToggle) << std::endl;

	auto active = &entity->transactions.front();
	for(int i = 0; i < active->transfers.size(); i++) {
		auto &transfer = active->transfers[i];
		std::cout << "transfer " << i << ": " << std::endl;
		std::cout << "    pingError: " << (int)(transfer.status.load()
				& td_status::pingError) << std::endl;
		std::cout << "    splitXState: " << (int)(transfer.status.load()
				& td_status::splitXState) << std::endl;
		std::cout << "    missedFrame: " << (int)(transfer.status.load()
				& td_status::missedFrame) << std::endl;
		std::cout << "    transactionError: " << (int)(transfer.status.load()
				& td_status::transactionError) << std::endl;
		std::cout << "    babbleDetected: " << (int)(transfer.status.load()
				& td_status::babbleDetected) << std::endl;
		std::cout << "    dataBufferError: " << (int)(transfer.status.load()
				& td_status::dataBufferError) << std::endl;
		std::cout << "    halted: " << (int)(transfer.status.load()
				& td_status::halted) << std::endl;
		std::cout << "    pidCode: " << (int)(transfer.status.load()
				& td_status::pidCode) << std::endl;
		std::cout << "    errorCounter: " << (int)(transfer.status.load()
				& td_status::errorCounter) << std::endl;
		std::cout << "    cPage: " << (int)(transfer.status.load()
				& td_status::cPage) << std::endl;
		std::cout << "    interruptOnComplete: " << (int)(transfer.status.load()
				& td_status::interruptOnComplete) << std::endl;
		std::cout << "    totalBytes: " << (int)(transfer.status.load()
				& td_status::totalBytes) << std::endl;
		std::cout << "    dataToggle: " << (int)(transfer.status.load()
				& td_status::dataToggle) << std::endl;
	}
}

// --------------------------------------------------------
// main() function
// --------------------------------------------------------

int main() {
	printf("Starting EHCI driver\n");

	observeControllers();

	while(true)
		helix::Dispatcher::global().dispatch();
	
	return 0;
}
 
