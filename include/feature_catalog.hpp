#pragma once
#include <string>
#include <vector>

enum class FeatureStatus { Implemented, Partial, Planned };
struct FeatureEntry {
    std::string section;
    std::string name;
    FeatureStatus status;
};

class FeatureCatalog {
  public:
    static const std::vector<FeatureEntry> &all();
    static std::vector<FeatureEntry> search(const std::string &query);
    static std::string statusName(FeatureStatus status);
    static bool exportText(const std::string &path, std::string &error);
    static bool exportJson(const std::string &path, std::string &error);
};
