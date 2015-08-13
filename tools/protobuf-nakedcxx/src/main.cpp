
#include <sstream>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/compiler/plugin.h>
#include <google/protobuf/compiler/code_generator.h>
#include <google/protobuf/io/zero_copy_stream.h>
#include <google/protobuf/io/printer.h>

namespace pb = google::protobuf;

void generateEnum(pb::io::Printer &printer, const pb::EnumDescriptor *enumeration) {
	printer.Print("struct $name$ {\n", "name", enumeration->name());
	printer.Indent();
	printer.Print("enum {\n");
	printer.Indent();

	for(int i = 0; i < enumeration->value_count(); i++) {
		const pb::EnumValueDescriptor *value = enumeration->value(i);
		printer.Print("$name$ = $number$", "name", value->name(),
				"number", std::to_string(value->number()));

		if(i + 1 < enumeration->value_count())
			printer.Print(",");
		printer.Print("\n");
	}

	printer.Outdent();
	printer.Print("};\n");
	printer.Outdent();
	printer.Print("};\n");
}

void generateMessage(pb::io::Printer &printer, const pb::Descriptor *descriptor) {
	// generate a containing class for each message
	printer.Print("struct $name$ {\n", "name", descriptor->name());
	printer.Indent();
	
	printer.Print("enum {\n");
	printer.Indent();
	
	for(int i = 0; i < descriptor->field_count(); i++) {
		const pb::FieldDescriptor *field = descriptor->field(i);
		printer.Print("kField_$name$ = $number$", "name", field->name(),
				"number", std::to_string(field->number()));

		if(i + 1 < descriptor->field_count())
			printer.Print(",");
		printer.Print("\n");
	}

	printer.Outdent();
	printer.Print("};\n");

	for(int i = 0; i < descriptor->enum_type_count(); i++) {
		printer.Print("\n");
		generateEnum(printer, descriptor->enum_type(i));
	}

	// generate a method that reads a message
	// close the containing class
	printer.Outdent();
	printer.Print("};\n");
}

class NakedCxxGenerator : public pb::compiler::CodeGenerator {
public:
	virtual bool Generate(const pb::FileDescriptor *file, const std::string &parameter,
			pb::compiler::GeneratorContext *context, std::string *error) const;
};

bool NakedCxxGenerator::Generate(const pb::FileDescriptor *file, const std::string &parameter,
			pb::compiler::GeneratorContext *context, std::string *error) const {
	std::string path = file->name();
	size_t file_dot = path.rfind('.');
	if(file_dot != std::string::npos)
		path.erase(path.begin() + file_dot, path.end());
	path += ".nakedpb.hpp";

	pb::io::ZeroCopyOutputStream *output = context->Open(path);
	{ // limit the lifetime of printer
		pb::io::Printer printer(output, '$');

		printer.Print("// This file is auto-generated from $file$\n",
				"file", file->name());
		printer.Print("// Do not try to edit it manually!\n");
		
		// print the namespace opening braces
		int num_namespaces = 0;
		std::string pkg_full, pkg_part;
		std::istringstream pkg_stream(file->package());
		printer.Print("\n");
		while(std::getline(pkg_stream, pkg_part, '.')) {
			printer.Print("namespace $pkg_part$ {\n", "pkg_part", pkg_part);
			pkg_full += (num_namespaces == 0 ? "" : "::") + pkg_part;
			num_namespaces++;
		}
		
		// generate all enums
		for(int i = 0; i < file->enum_type_count(); i++) {
			printer.Print("\n");
			generateEnum(printer, file->enum_type(i));
		}
		
		// generate all messages
		for(int i = 0; i < file->message_type_count(); i++) {
			printer.Print("\n");
			generateMessage(printer, file->message_type(i));
		}

		// print the closing braces for the namespace
		printer.Print("\n");
		for(int i = 0; i < num_namespaces; i++)
			printer.Print("} ");
		if(num_namespaces > 0)
			printer.Print("// namespace $pkg_full$\n", "pkg_full", pkg_full);
	} // printer is destructed here
	delete output;

	return true;
}

int main(int argc, char* argv[]) {
	NakedCxxGenerator generator;
	return pb::compiler::PluginMain(argc, argv, &generator);
}

