
namespace thor {

class RdFolder {
public:
	enum {
		kNameLength = 32
	};

	enum Type {
		kTypeNone,
		kTypeMounted,
		kTypeDescriptor
	};
	
	struct Entry {
		Entry(Type type);
		
		Type type;
		char name[kNameLength];
		size_t nameLength;
		SharedPtr<RdFolder, KernelAlloc> mounted;
		AnyDescriptor descriptor;
	};

	RdFolder();

	void mount(const char *name, size_t name_length,
			SharedPtr<RdFolder, KernelAlloc> &&mounted);
	void publish(const char *name, size_t name_length,
			AnyDescriptor &&descriptor);
	
	Entry *getEntry(const char *name, size_t name_length);

private:
	frigg::util::Vector<Entry, KernelAlloc> p_entries;
};

} // namespace thor

