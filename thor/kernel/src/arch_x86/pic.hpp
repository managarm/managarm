
namespace thor {

// --------------------------------------------------------
// Local APIC management
// --------------------------------------------------------

void initializeLocalApic();

void calibrateApicTimer();

void raiseInitAssertIpi(uint32_t dest_apic_id);

void raiseInitDeassertIpi(uint32_t dest_apic_id);

void raiseStartupIpi(uint32_t dest_apic_id, uint32_t page);

// --------------------------------------------------------
// Legacy PIC management
// --------------------------------------------------------

void setupLegacyPic();

void acknowledgeIrq(int irq);

} // namespace thor

