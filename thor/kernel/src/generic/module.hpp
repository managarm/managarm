
// TODO: add a proper include guard and sort this into the other kernel headers

namespace thor {

struct Module {
	Module(frigg::StringView filename, PhysicalAddr physical, size_t length)
	: filename(filename), physical(physical), length(length) { }

	frigg::StringView filename;
	PhysicalAddr physical;
	size_t length;
};

extern frigg::LazyInitializer<frigg::Vector<Module, KernelAlloc>> allModules;

Module *getModule(frigg::StringView filename);

} // namespace thor

