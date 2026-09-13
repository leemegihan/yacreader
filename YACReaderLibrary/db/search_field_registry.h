#ifndef SEARCH_FIELD_REGISTRY_H
#define SEARCH_FIELD_REGISTRY_H

#include <QList>
#include <QString>

#include <map>
#include <string>
#include <vector>

enum class SearchFieldType {
    Unknown,
    Numeric,
    Text,
    MetadataToken,
    Boolean,
    Date,
    DateFolder,
    Folder,
    BooleanFolder,
    Filename,
    EnumField,
    EnumFieldFolder
};

enum class SearchFieldCategory {
    Common,
    Credits,
    Story,
    Publication,
    ReadingAndFiles,
    Folders
};

enum class SearchFieldScope {
    Comics,
    Folders,
    ComicsAndFolders
};

struct SearchFieldDefinition {
    QString key;
    QString displayName;
    QString description;
    QString input;
    QString example;
    SearchFieldType type;
    SearchFieldCategory category;
    SearchFieldScope scope;
};

const std::map<SearchFieldType, std::vector<std::string>> &searchFieldNames();
SearchFieldType searchFieldType(const std::string &name);
const QList<SearchFieldDefinition> &searchFieldDefinitions();

#endif // SEARCH_FIELD_REGISTRY_H
