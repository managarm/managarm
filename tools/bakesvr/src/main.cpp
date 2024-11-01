#include <CLI/App.hpp>
#include <CLI/Config.hpp>
#include <CLI/Formatter.hpp>
#include <string>
#include <svrctl.bragi.hpp>
#include <vector>
#include <yaml-cpp/yaml.h>

int main(int argc, char **argv) {
	std::string input{};
	std::string output{};

	CLI::App app{"bakesvr: generate info for runsvr"};
	app.add_option("input", input, "Path to the input file")->required();
	app.add_option("-o,--output", output, "Path to the output file")->required();
	CLI11_PARSE(app, argc, argv);

	auto config = YAML::LoadFile(input);

	managarm::svrctl::Description data{};
	data.set_name(config["name"].as<std::string>());
	data.set_exec(config["exec"].as<std::string>());
	for (size_t i = 0; i < config["files"].size(); i++) {
		managarm::svrctl::File f{};
		f.set_path(config["files"][i].as<std::string>());
		data.add_files(f);
	}

	std::vector<char> buf(data.size_of_body());
	bragi::limited_writer wr{buf.data(), data.size_of_body()};
	auto sr = bragi::serializer{};
	data.encode_body(wr, sr);

	std::ofstream out{output, std::ios::binary};
	out.write(buf.data(), data.size_of_body());

	return 0;
}
