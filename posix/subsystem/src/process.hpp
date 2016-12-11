
#ifndef POSIX_SUBSYSTEM_PROCESS_HPP
#define POSIX_SUBSYSTEM_PROCESS_HPP

#include <memory>
#include <unordered_map>

typedef int ProcessId;

struct SharedProcess {
	static SharedProcess createInit() {
		auto data = std::make_shared<Data>();
		return SharedProcess(std::move(data));
	}

	int attachFile(std::shared_ptr<File> file) const {
		for(int fd = 0; ; fd++) {
			if(_data->fileTable.find(fd) != _data->fileTable.end())
				continue;
			_data->fileTable.insert({ fd, std::move(file) });
			return fd;
		}
	}
	
	void attachFile(int fd, std::shared_ptr<File> file) const {
		auto it = _data->fileTable.find(fd);
		if(it != _data->fileTable.end()) {
			it->second = std::move(file);
		}else{
			_data->fileTable.insert({ fd, std::move(file) });
		}
	}

	std::shared_ptr<File> getFile(int fd) const {
		return _data->fileTable.at(fd);
	}

private:
	struct Data {
		// TODO: replace this by a tree that remembers gaps between keys.
		std::unordered_map<int, std::shared_ptr<File>> fileTable;
	};

	explicit SharedProcess(std::shared_ptr<Data> data)
	: _data(std::move(data)) { }

	std::shared_ptr<Data> _data;
};

#endif // POSIX_SUBSYSTEM_PROCESS_HPP

