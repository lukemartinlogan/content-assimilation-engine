#include "OMNI.h"
#include "OMNI_factory.h"
#include "filesystem_repo_omni.h"
#include <iostream>
#include <mpi.h>
#include <sstream>
#include <string>
#include <vector>
#include <yaml-cpp/yaml.h>

using namespace cae;

struct OmniJobConfig {
  std::string name;
  int max_scale;

  struct DataEntry {
    std::string path;
    std::vector<size_t> range;
    size_t offset;
    size_t size;
    std::vector<std::string> description;
    std::string hash;

    DataEntry() : offset(0), size(0) {}
  };

  std::vector<DataEntry> data_entries;

  OmniJobConfig() : max_scale(100) {}
};

OmniJobConfig ParseOmniFile(const std::string &yaml_file) {
  OmniJobConfig config;

  try {
    YAML::Node yaml = YAML::LoadFile(yaml_file);

    if (yaml["name"]) {
      config.name = yaml["name"].as<std::string>();
    }

    if (yaml["max_scale"]) {
      config.max_scale = yaml["max_scale"].as<int>();
    }

    if (yaml["data"]) {
      const YAML::Node &data_node = yaml["data"];
      for (const auto &entry : data_node) {
        OmniJobConfig::DataEntry data_entry;

        if (entry["path"]) {
          data_entry.path = entry["path"].as<std::string>();
        }

        if (entry["range"]) {
          const YAML::Node &range_node = entry["range"];
          for (const auto &val : range_node) {
            data_entry.range.push_back(val.as<size_t>());
          }
        }

        if (entry["offset"]) {
          data_entry.offset = entry["offset"].as<size_t>();
        }

        if (entry["size"]) {
          data_entry.size = entry["size"].as<size_t>();
        }

        if (entry["description"]) {
          const YAML::Node &desc_node = entry["description"];
          for (const auto &desc : desc_node) {
            data_entry.description.push_back(desc.as<std::string>());
          }
        }

        if (entry["hash"]) {
          data_entry.hash = entry["hash"].as<std::string>();
        }

        // If size is not specified (0), automatically detect file size
        if (data_entry.size == 0 && !data_entry.path.empty()) {
          FilesystemRepoClient fs_client;
          size_t file_size = fs_client.GetFileSize(data_entry.path);
          if (file_size > 0) {
            // Calculate size as file_size - offset to read from offset to end
            // of file
            if (data_entry.offset < file_size) {
              data_entry.size = file_size - data_entry.offset;
              std::cout << "Auto-detected size for " << data_entry.path << ": "
                        << data_entry.size << " bytes (from offset "
                        << data_entry.offset << " to end of file)" << std::endl;
            } else {
              std::cerr << "Warning: Offset " << data_entry.offset
                        << " is beyond file size " << file_size << " for "
                        << data_entry.path << std::endl;
            }
          } else {
            std::cerr << "Warning: Could not determine file size for "
                      << data_entry.path << std::endl;
          }
        }

        // Derive range from offset and size if range is not specified
        if (data_entry.range.empty() && data_entry.size > 0) {
          data_entry.range.push_back(data_entry.offset);
          data_entry.range.push_back(data_entry.offset + data_entry.size);
        }

        config.data_entries.push_back(data_entry);
      }
    }

  } catch (const YAML::Exception &e) {
    std::cerr << "YAML parsing error: " << e.what() << std::endl;
    throw;
  }

  return config;
}

std::string BuildBinaryCommand(const OmniJobConfig::DataEntry &entry,
                               int nprocs) {
  std::ostringstream cmd;

  // Try MPI first, fall back to direct execution
  cmd << "which mpirun >/dev/null 2>&1 && ";
  cmd << "mpirun -np " << nprocs << " bin/binary_file_omni";
  cmd << " \"" << entry.path << "\"";
  cmd << " " << entry.offset;
  cmd << " " << entry.size;

  if (!entry.description.empty()) {
    cmd << " \"";
    for (size_t i = 0; i < entry.description.size(); ++i) {
      if (i > 0)
        cmd << ",";
      cmd << entry.description[i];
    }
    cmd << "\"";
  }

  if (!entry.hash.empty()) {
    cmd << " \"" << entry.hash << "\"";
  }

  // Fallback command without MPI
  cmd << " || bin/binary_file_omni";
  cmd << " \"" << entry.path << "\"";
  cmd << " " << entry.offset;
  cmd << " " << entry.size;

  if (!entry.description.empty()) {
    cmd << " \"";
    for (size_t i = 0; i < entry.description.size(); ++i) {
      if (i > 0)
        cmd << ",";
      cmd << entry.description[i];
    }
    cmd << "\"";
  }

  if (!entry.hash.empty()) {
    cmd << " \"" << entry.hash << "\"";
  }

  return cmd.str();
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0] << " <omni_yaml_file>" << std::endl;
    return 1;
  }

  try {
    // Parse the OMNI YAML file
    OmniJobConfig config = ParseOmniFile(argv[1]);

    std::cout << "Parsed OMNI job: " << config.name << std::endl;
    std::cout << "Max scale: " << config.max_scale << std::endl;
    std::cout << "Number of data entries: " << config.data_entries.size()
              << std::endl;

    // Process each data entry
    for (size_t i = 0; i < config.data_entries.size(); ++i) {
      const auto &entry = config.data_entries[i];

      std::cout << "\nProcessing data entry " << (i + 1) << ": " << entry.path
                << std::endl;

      // Use filesystem repo client to recommend scale
      FilesystemRepoClient fs_client;
      int nprocs, nthreads;
      fs_client.RecommendScaleForFile(entry.path, config.max_scale, nprocs,
                                      nthreads);

      // Build and execute the command (with MPI fallback)
      std::string command = BuildBinaryCommand(entry, nprocs);
      std::cout << "Executing: " << command << std::endl;

      int result = system(command.c_str());
      if (result != 0) {
        std::cerr << "Warning: Command completed with exit code " << result
                  << std::endl;
        // Continue processing other entries instead of failing completely
      }
    }

    std::cout << "\nAll data entries processed!" << std::endl;

  } catch (const std::exception &e) {
    std::cerr << "Error: " << e.what() << std::endl;
    return 1;
  }

  return 0;
}