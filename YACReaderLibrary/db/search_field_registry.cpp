#include "search_field_registry.h"

#include <QCoreApplication>

#include <algorithm>
#include <cctype>

namespace {

QString inputDescription(SearchFieldType type)
{
    switch (type) {
    case SearchFieldType::MetadataToken:
        return QCoreApplication::translate("SearchFieldRegistry", "Exact author or tag, quoted if it contains spaces");
    case SearchFieldType::Text:
    case SearchFieldType::Filename:
    case SearchFieldType::Folder:
        return QCoreApplication::translate("SearchFieldRegistry", "Text, quoted text");
    case SearchFieldType::Numeric:
        return QCoreApplication::translate("SearchFieldRegistry", "Integer");
    case SearchFieldType::Boolean:
    case SearchFieldType::BooleanFolder:
        return QCoreApplication::translate("SearchFieldRegistry", "Boolean (true / false)");
    case SearchFieldType::Date:
    case SearchFieldType::DateFolder:
        return QCoreApplication::translate("SearchFieldRegistry", "Integer (number of days)");
    case SearchFieldType::EnumField:
    case SearchFieldType::EnumFieldFolder:
        return QCoreApplication::translate("SearchFieldRegistry", "Enum (comic, manga, westernmanga, webcomic/web, 4koma/yonkoma)");
    case SearchFieldType::Unknown:
        return { };
    }

    return { };
}

SearchFieldDefinition field(
        const char *key,
        const char *displayName,
        const char *description,
        const char *example,
        SearchFieldType type,
        SearchFieldCategory category,
        SearchFieldScope scope = SearchFieldScope::Comics)
{
    return {
        QString::fromLatin1(key),
        QString::fromUtf8(displayName),
        QCoreApplication::translate("SearchFieldRegistry", description),
        inputDescription(type),
        QString::fromLatin1(example),
        type,
        category,
        scope
    };
}

}

const std::map<SearchFieldType, std::vector<std::string>> &searchFieldNames()
{
    static const std::map<SearchFieldType, std::vector<std::string>> names {
        { SearchFieldType::MetadataToken, { "author", "tag" } },
        { SearchFieldType::Numeric, { "numpages", "count", "arccount", "alternateCount", "rating" } },
        { SearchFieldType::Text, { "date", "number", "arcnumber", "title", "volume", "storyarc", "genere", "writer", "penciller", "inker", "colorist", "letterer", "coverartist", "publisher", "format", "agerating", "synopsis", "characters", "notes", "editor", "imprint", "teams", "locations", "series", "alternateSeries", "alternateNumber", "languageISO", "seriesGroup", "mainCharacterOrTeam", "review", "tags" } },
        { SearchFieldType::Boolean, { "color", "read", "edited", "hasBeenOpened" } },
        { SearchFieldType::Date, { "added", "lastTimeOpened" } },
        { SearchFieldType::DateFolder, { "added", "updated" } },
        { SearchFieldType::Filename, { "filename" } },
        { SearchFieldType::Folder, { "folder" } },
        { SearchFieldType::BooleanFolder, { "completed", "finished" } },
        { SearchFieldType::EnumField, { "type" } },
        { SearchFieldType::EnumFieldFolder, { "foldertype" } }
    };

    return names;
}

SearchFieldType searchFieldType(const std::string &name)
{
    std::string lowerName(name);
    std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), [](unsigned char c) { return std::tolower(c); });

    for (const auto &[type, names] : searchFieldNames()) {
        for (const auto &candidate : names) {
            std::string lowerCandidate(candidate);
            std::transform(lowerCandidate.begin(), lowerCandidate.end(), lowerCandidate.begin(), [](unsigned char c) { return std::tolower(c); });
            if (lowerCandidate == lowerName)
                return type;
        }
    }

    return SearchFieldType::Unknown;
}

const QList<SearchFieldDefinition> &searchFieldDefinitions()
{
    using Category = SearchFieldCategory;
    using Scope = SearchFieldScope;
    using Type = SearchFieldType;

    static const QList<SearchFieldDefinition> definitions {
        field("title", "Title", QT_TRANSLATE_NOOP("SearchFieldRegistry", "Comic title"), "title:Moonbound", Type::Text, Category::Common),
        field("series", "Series", QT_TRANSLATE_NOOP("SearchFieldRegistry", "Series name"), "series:\"Starfall Chronicles\"", Type::Text, Category::Common),
        field("number", "Number", QT_TRANSLATE_NOOP("SearchFieldRegistry", "Issue number"), "number>=10", Type::Text, Category::Common),
        field("volume", "Volume", QT_TRANSLATE_NOOP("SearchFieldRegistry", "Volume identifier"), "volume:2", Type::Text, Category::Common),
        field("type", "Comic type", QT_TRANSLATE_NOOP("SearchFieldRegistry", "Reading format"), "type:manga", Type::EnumField, Category::Common),
        field("rating", "Rating", QT_TRANSLATE_NOOP("SearchFieldRegistry", "Comic rating"), "rating>=4", Type::Numeric, Category::Common),
        field("author", "Author (exact)", QT_TRANSLATE_NOOP("SearchFieldRegistry", "Exact author in a list of credits"), "author:\"Alex Smith\"", Type::MetadataToken, Category::Credits),
        field("tag", "Tag (exact)", QT_TRANSLATE_NOOP("SearchFieldRegistry", "Exact tag; combine with AND, OR, NOT"), "tag:Action AND tag:Fantasy", Type::MetadataToken, Category::Common),
        field("tags", "Tags", QT_TRANSLATE_NOOP("SearchFieldRegistry", "Textual tags"), "tags:\"to review\"", Type::Text, Category::Common),

        field("writer", "Writer", QT_TRANSLATE_NOOP("SearchFieldRegistry", "Writer credit"), "writer:Smith", Type::Text, Category::Credits),
        field("penciller", "Penciller", QT_TRANSLATE_NOOP("SearchFieldRegistry", "Penciller credit"), "penciller:\"Alex Smith\"", Type::Text, Category::Credits),
        field("inker", "Inker", QT_TRANSLATE_NOOP("SearchFieldRegistry", "Inker credit"), "inker:\"Taylor Reed\"", Type::Text, Category::Credits),
        field("colorist", "Colorist", QT_TRANSLATE_NOOP("SearchFieldRegistry", "Colorist credit"), "colorist:\"Morgan Lane\"", Type::Text, Category::Credits),
        field("letterer", "Letterer", QT_TRANSLATE_NOOP("SearchFieldRegistry", "Letterer credit"), "letterer:\"Casey Brooks\"", Type::Text, Category::Credits),
        field("coverartist", "Cover artist", QT_TRANSLATE_NOOP("SearchFieldRegistry", "Cover artist credit"), "coverartist:\"Jordan Blake\"", Type::Text, Category::Credits),
        field("editor", "Editor", QT_TRANSLATE_NOOP("SearchFieldRegistry", "Editor credit"), "editor:\"Avery Stone\"", Type::Text, Category::Credits),

        field("storyarc", "Story arc", QT_TRANSLATE_NOOP("SearchFieldRegistry", "Story arc name"), "storyarc:Afterlight", Type::Text, Category::Story),
        field("arcnumber", "Arc number", QT_TRANSLATE_NOOP("SearchFieldRegistry", "Position within a story arc"), "arcnumber>=2", Type::Text, Category::Story),
        field("arccount", "Arc count", QT_TRANSLATE_NOOP("SearchFieldRegistry", "Number of issues in a story arc"), "arccount>5", Type::Numeric, Category::Story),
        field("characters", "Characters", QT_TRANSLATE_NOOP("SearchFieldRegistry", "Characters appearing in the comic"), "characters:Solara", Type::Text, Category::Story),
        field("teams", "Teams", QT_TRANSLATE_NOOP("SearchFieldRegistry", "Teams appearing in the comic"), "teams:\"Aurora Guard\"", Type::Text, Category::Story),
        field("locations", "Locations", QT_TRANSLATE_NOOP("SearchFieldRegistry", "Locations appearing in the comic"), "locations:Greyhaven", Type::Text, Category::Story),
        field("mainCharacterOrTeam", "Main character or team", QT_TRANSLATE_NOOP("SearchFieldRegistry", "Primary character or team"), "mainCharacterOrTeam:Solara", Type::Text, Category::Story),
        field("synopsis", "Synopsis", QT_TRANSLATE_NOOP("SearchFieldRegistry", "Comic synopsis"), "synopsis:\"parallel world\"", Type::Text, Category::Story),

        field("publisher", "Publisher", QT_TRANSLATE_NOOP("SearchFieldRegistry", "Publisher name"), "publisher:ExamplePress", Type::Text, Category::Publication),
        field("imprint", "Imprint", QT_TRANSLATE_NOOP("SearchFieldRegistry", "Publishing imprint"), "imprint:\"Silver Line\"", Type::Text, Category::Publication),
        field("format", "Format", QT_TRANSLATE_NOOP("SearchFieldRegistry", "Publication format"), "format:annual", Type::Text, Category::Publication),
        field("agerating", "Age rating", QT_TRANSLATE_NOOP("SearchFieldRegistry", "Recommended age rating"), "agerating:Teen", Type::Text, Category::Publication),
        field("genere", "Genre", QT_TRANSLATE_NOOP("SearchFieldRegistry", "Comic genre"), "genere:Horror", Type::Text, Category::Publication),
        field("languageISO", "Language", QT_TRANSLATE_NOOP("SearchFieldRegistry", "ISO language code"), "languageISO:en", Type::Text, Category::Publication),
        field("date", "Publication date", QT_TRANSLATE_NOOP("SearchFieldRegistry", "Publication date metadata"), "date:2024", Type::Text, Category::Publication),
        field("seriesGroup", "Series group", QT_TRANSLATE_NOOP("SearchFieldRegistry", "Series grouping metadata"), "seriesGroup:\"Pocket Editions\"", Type::Text, Category::Publication),
        field("alternateSeries", "Alternate series", QT_TRANSLATE_NOOP("SearchFieldRegistry", "Alternate series name"), "alternateSeries:\"Midnight Tales\"", Type::Text, Category::Publication),
        field("alternateNumber", "Alternate number", QT_TRANSLATE_NOOP("SearchFieldRegistry", "Alternate issue number"), "alternateNumber>=10", Type::Text, Category::Publication),
        field("alternateCount", "Alternate count", QT_TRANSLATE_NOOP("SearchFieldRegistry", "Alternate series issue count"), "alternateCount>20", Type::Numeric, Category::Publication),
        field("count", "Issue count", QT_TRANSLATE_NOOP("SearchFieldRegistry", "Number of issues in the series"), "count>=12", Type::Numeric, Category::Publication),

        field("read", "Read", QT_TRANSLATE_NOOP("SearchFieldRegistry", "Whether the comic is marked as read"), "read:false", Type::Boolean, Category::ReadingAndFiles),
        field("hasBeenOpened", "Has been opened", QT_TRANSLATE_NOOP("SearchFieldRegistry", "Whether reading has started"), "hasBeenOpened:true", Type::Boolean, Category::ReadingAndFiles),
        field("edited", "Edited", QT_TRANSLATE_NOOP("SearchFieldRegistry", "Whether metadata has been edited"), "edited:true", Type::Boolean, Category::ReadingAndFiles),
        field("color", "Color", QT_TRANSLATE_NOOP("SearchFieldRegistry", "Whether the comic is in color"), "color:true", Type::Boolean, Category::ReadingAndFiles),
        field("numpages", "Page count", QT_TRANSLATE_NOOP("SearchFieldRegistry", "Number of pages"), "numpages>100", Type::Numeric, Category::ReadingAndFiles),
        field("filename", "File name", QT_TRANSLATE_NOOP("SearchFieldRegistry", "Comic file name"), "filename:cbz", Type::Filename, Category::ReadingAndFiles),
        field("added", "Date added", QT_TRANSLATE_NOOP("SearchFieldRegistry", "When the item was added"), "added>7", Type::Date, Category::ReadingAndFiles, Scope::ComicsAndFolders),
        field("lastTimeOpened", "Last opened", QT_TRANSLATE_NOOP("SearchFieldRegistry", "When the comic was last opened"), "lastTimeOpened>30", Type::Date, Category::ReadingAndFiles),
        field("notes", "Notes", QT_TRANSLATE_NOOP("SearchFieldRegistry", "Comic notes"), "notes:\"variant cover\"", Type::Text, Category::ReadingAndFiles),
        field("review", "Review", QT_TRANSLATE_NOOP("SearchFieldRegistry", "Review text"), "review:\"highly recommended\"", Type::Text, Category::ReadingAndFiles),

        field("folder", "Folder", QT_TRANSLATE_NOOP("SearchFieldRegistry", "Parent folder name"), "folder:\"Example Comics\"", Type::Folder, Category::Folders, Scope::Folders),
        field("foldertype", "Folder type", QT_TRANSLATE_NOOP("SearchFieldRegistry", "Default reading format for the folder"), "foldertype:manga", Type::EnumFieldFolder, Category::Folders, Scope::Folders),
        field("completed", "Completed", QT_TRANSLATE_NOOP("SearchFieldRegistry", "Whether the folder is complete"), "completed:true", Type::BooleanFolder, Category::Folders, Scope::Folders),
        field("finished", "Finished", QT_TRANSLATE_NOOP("SearchFieldRegistry", "Whether the folder is marked as finished"), "finished:true", Type::BooleanFolder, Category::Folders, Scope::Folders),
        field("updated", "Date updated", QT_TRANSLATE_NOOP("SearchFieldRegistry", "When the folder was updated"), "updated>7", Type::DateFolder, Category::Folders, Scope::Folders)
    };

    return definitions;
}
