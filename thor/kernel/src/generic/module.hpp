
// TODO: add a proper include guard and sort this into the other kernel headers

namespace thor {

struct Module {
	Module(frigg::StringView filename, frigg::SharedPtr<Memory> memory)
	: filename(filename), memory(frigg::move(memory)) { }

	frigg::StringView filename;
	frigg::SharedPtr<Memory> memory;
};

extern frigg::LazyInitializer<frigg::Vector<Module, KernelAlloc>> allModules;

Module *getModule(frigg::StringView filename);

} // namespace thor

