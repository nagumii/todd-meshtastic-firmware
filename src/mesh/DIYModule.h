#pragma once

#include "FSCommon.h"
#include "MeshModule.h"

class DIYModule : MeshModule
{
    static std::vector<DIYModule *> *diyModules;

  public:
    enum ControlStyle { BY_NAME, OWN_CHANNEL };
    DIYModule(const char *_name, ControlStyle style);

    static ProcessMessage interceptSentText(meshtastic_MeshPacket &mp, RxSource src);
    virtual void handleSentText(const meshtastic_MeshPacket &mp) {}

  protected:
    void sendPhoneFeedback(const char *text, const char *channelName = "");
    static bool stringsMatch(const char *s1, const char *s2, bool caseSensitive = true);
    static char *getArg(uint8_t index, bool untilEnd = false);

    static bool channelExists(const char *channelName);
    static bool isFromChannel(const meshtastic_MeshPacket &mp, const char *channelName);
    static bool isFromPublicChannel(const meshtastic_MeshPacket &mp);
    static const char *getChannelName(const meshtastic_MeshPacket &mp);
    bool parseBool(const char *raw);

    ControlStyle style;
    char ownChannelName[12]{0};

    // Dynamic memory. Used by getArg. Freed at end of interceptSentText
    static char *currentText;
    static char *requestedArg;

    template <typename T> void loadData(T *data);
    template <typename T> void saveData(T *data);
    uint32_t getDataHash(void *data, uint32_t size);

    const char *saveDirectory = "/DIYModules";
};

// ======================
//    Template methods
// ======================

template <typename T> void DIYModule::loadData(T *data)
{
    // Build the filepath using the module's name
    String filename = saveDirectory;
    filename += "/";
    filename += name;
    filename += ".data";

#ifdef FSCom

    // Check that the file *does* actually exist
    if (!FSCom.exists(filename)) {
        LOG_INFO("'%s' not found. Using default values\n", filename.c_str());
        // Todo: init struct
        return;
    }

    // Open the file
    auto f = FSCom.open(filename, FILE_O_READ);

    // If opened, start reading
    if (f) {
        LOG_INFO("Loading DIY module data '%s'\n", filename.c_str());

        // Read the actual data
        f.readBytes((char *)data, sizeof(T));

        // Read the hash
        uint32_t savedHash = 0;
        f.readBytes((char *)&savedHash, sizeof(savedHash));

        // Calculate hash of the loaded data, then compare with the saved hash
        uint32_t calculatedHash = getDataHash(data, sizeof(T));
        if (savedHash != calculatedHash) {
            LOG_WARN("'%s' is corrupt (hash mismatch). Using default values\n", filename.c_str());
            *data = T(); // Reinit the data object
        }

        f.close();
    } else {
        LOG_ERROR("Could not open / read %s\n", filename.c_str());
    }
#else
    LOG_ERROR("ERROR: Filesystem not implemented\n");
    state = LoadFileState::NO_FILESYSTEM;
#endif
    return;
}

template <typename T> void DIYModule::saveData(T *data)
{
    // Build the filepath using the module's name
    String filename = saveDirectory;
    filename += "/";
    filename += name;
    filename += ".data";

#ifdef FSCom
    // Make the directory, if it doesn't exist
    if (!FSCom.exists(saveDirectory))
        FSCom.mkdir(saveDirectory);

    // Create a temporary filename, where we will write data, then later rename
    String filenameTmp = filename;
    filenameTmp += ".tmp";

    // Open the file
    auto f = FSCom.open(filenameTmp.c_str(), FILE_O_WRITE);

    // If it opened, start writing
    if (f) {
        LOG_INFO("Saving DIY module data '%s'\n", filename.c_str());

        // Calculate a hash of the data
        uint32_t hash = getDataHash(data, sizeof(T));

        f.write((uint8_t *)data, sizeof(T));     // Write the actualy data
        f.write((uint8_t *)&hash, sizeof(hash)); // Append the hash

        f.flush();
        f.close();

        // Remove the old file (brief window of risk here()
        if (FSCom.exists(filename) && !FSCom.remove(filename))
            LOG_WARN("Can't remove old DIY module file '%s'\n", filename.c_str());

        // Rename the new (temporary) file to take place of the old
        if (!renameFile(filenameTmp.c_str(), filename.c_str()))
            LOG_ERROR("Error: can't rename new  DIY module file '%s\n", filename.c_str());
    } else {
        LOG_ERROR("Can't write DIY module file '%s'\n", filenameTmp.c_str());
#ifdef ARCH_NRF52
        static uint8_t failedCounter = 0;
        failedCounter++;
        if (failedCounter >= 2) {
            LOG_ERROR("Failed to save DIY module file twice. Rebooting...\n");
            delay(100);
            NVIC_SystemReset();
        } else
            saveConfig(); // Recurse
#endif
    }
#else
    LOG_ERROR("ERROR: Filesystem not implemented\n");
#endif
}