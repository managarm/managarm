
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
		KernelSharedPtr<RdFolder> mounted;
		AnyDescriptor descriptor;
	};

	RdFolder();

	void mount(const char *name, size_t name_length,
			KernelSharedPtr<RdFolder> &&mounted);
	void publish(const char *name, size_t name_length,
			AnyDescriptor &&descriptor);
	
	frigg::Optional<Entry *> getEntry(const char *name, size_t name_length);

private:
	frigg::Vector<Entry, KernelAlloc> p_entries;
};

} // namespace thor

