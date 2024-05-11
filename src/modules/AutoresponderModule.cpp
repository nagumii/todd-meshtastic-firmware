#include "AutoresponderModule.h"

#if MESHTASTIC_INCLUDE_DIYMODULES && MESHTASTIC_INCLUDE_AUTORESPONDER

#include "MeshService.h"
#include "NodeDB.h"
#include "Router.h"
#include "configuration.h"
#include "graphics/Screen.h"
#include "main.h"

// Fixed limits: Channel
static constexpr uint8_t maxResponsesChannelDaily = 10; // Max responses per day, in-channel
static constexpr uint8_t expireAfterBootNum = 5;      // How many boots before response auto-disabled (channel and optionally, DM)
static constexpr uint16_t cooldownChannelMinutes = 2; // Minimum interval between ANY response in-channel

// Fixed limits: DM
static constexpr uint32_t repeatDMMinutes = 2; // How long to wait before allowing response to same node - in DM
// expireAfterBootNum also applies to DM, if autoresponder.should_dm_expire

// Limits on user-config: Channel
static constexpr uint32_t minRepeatPubChanHours = 8;  // How long to wait before allowing response to same node - public channel
static constexpr uint32_t minRepeatPrivChanHours = 4; // How long to wait before allowing response to same node - private channel
static constexpr uint32_t maxExpirationChannelHours = 72; // How long before module auto-disables in-channel responses

// Config for module (saved to flash)
struct AutoresponderConfig {
    bool enabled_dm = false;
    bool enabled_in_channel = false;
    uint32_t bootcount = 0;
    uint32_t repeat_hours = 0;
    uint32_t expiration_hours = 1;
    bool should_dm_expire = false;
    uint32_t permitted_nodes[8] = {0};
    uint8_t permitted_nodes_count = 0;
    char message[200] = "";
};

AutoresponderConfig arConfig;

// Constructor
AutoresponderModule::AutoresponderModule() : DIYModule("Autoresponder", ControlStyle::OWN_CHANNEL), OSThread("Autoresponder")
{
    // Load the module's data from flash
    loadData<AutoresponderConfig>(&arConfig);

    // If module is enabled
    if (arConfig.enabled_dm || arConfig.enabled_in_channel) {

        // Check if the node has rebooted frequently, in case bypassing rate limits and spamming the mesh
        bootCounting();

        // Cache the current channel name, to detect changes (can happen without reboot)
        strcpy(channelName, channels.getByIndex(0).settings.name);

        // Debug output at boot
        if (arConfig.enabled_dm)
            LOG_INFO("Autoresponder module enabled for DMs\n");
        if (arConfig.enabled_in_channel)
            LOG_INFO("Autoresponder: module enabled in channel\n");
        if (arConfig.permitted_nodes_count > 0) {
            LOG_INFO("Autoresponder: only responding to node ID ");
            for (uint8_t i = 0; i < arConfig.permitted_nodes_count; i++) {
                LOG_INFO("!%0x", arConfig.permitted_nodes[i]);
                if (i < arConfig.permitted_nodes_count - 1)
                    LOG_DEBUG(", ");
            }
            LOG_DEBUG("\n");
        }
    }

    // If module is disabled
    else
        LOG_INFO("Autoresponder module disabled\n");
}

// Do we want to process this packet with handleReceived()?
bool AutoresponderModule::wantPacket(const meshtastic_MeshPacket *p)
{
    // If module is disabled for both DM and in channel, ignore packets
    if (!arConfig.enabled_dm && !arConfig.enabled_in_channel)
        return false;

    // Which port is the packet from
    switch (p->decoded.portnum) {
    case meshtastic_PortNum_TEXT_MESSAGE_APP: // Text messages
        return true;
    case meshtastic_PortNum_ROUTING_APP: // Routing (looking for ACKs)
        return waitingForAck;
    default:
        return false;
    }
}

// MeshModule packets arrive here. Hand off the appropriate module
ProcessMessage AutoresponderModule::handleReceived(const meshtastic_MeshPacket &mp)
{
    // Hand off to relevant methood, basded on port number
    switch (mp.decoded.portnum) {
    case meshtastic_PortNum_TEXT_MESSAGE_APP: // Text message
        if (mp.to == myNodeInfo.my_node_num)
            handleDM(mp);
        else
            handleChannel(mp);
        break;
    case meshtastic_PortNum_ROUTING_APP: // Routing (for ACKs)
        checkForAck(mp);
        break;
    default:
        break;
    }

    return ProcessMessage::CONTINUE; // Let others look at this message also if they want
}

// Messages sent to the "Autorespond" channel
void AutoresponderModule::handleSentText(const meshtastic_MeshPacket &mp)
{

    char *command = getArg(0);

    // Set config
    if (stringsMatch(command, "set", false)) {
        char *option = getArg(1);

        if (stringsMatch(option, "message"))
            setMessage(getArg(2, true)); //"true" - until end of input

        else if (stringsMatch(option, "permitted_nodes")) {
            const char *value = getArg(2, true);

            if (stringsMatch(value, "all", false)) // Clear permitted nodes
                setPermittedNodes("");

            else
                setPermittedNodes(value); // Set permitted nodes
        }

        else if (stringsMatch(option, "repeat_hours"))
            setRepeatHours(atoi(getArg(2)));

        else if (stringsMatch(option, "expiration_hours"))
            setExpirationHours(atoi(getArg(2)));

        else if (stringsMatch(option, "should_dm_expire"))
            setShouldDmExpire(parseBool(getArg(2)));
    }

    // Enable
    else if (stringsMatch(command, "enable", false)) {
        char *where = getArg(1, true);

        if (strlen(where) == 0)
            setEnabled(true);

        else if (stringsMatch(where, "dm", false) || stringsMatch(where, "dms", false))
            setEnabledDM(true);

        else if (stringsMatch(where, "channel", false) || stringsMatch(where, "in channel"))
            setEnabledChannel(true);
    }

    // Disable
    else if (stringsMatch(command, "disable", false)) {
        char *where = getArg(1, true);

        if (strlen(where) == 0)
            setEnabled(false);

        else if (stringsMatch(where, "dm", false) || stringsMatch(where, "dms", false))
            setEnabledDM(false);

        else if (stringsMatch(where, "channel", false) || stringsMatch(where, "in channel"))
            setEnabledChannel(false);
    }

    // Help
    else if (stringsMatch(command, "help", false)) {
        printHelp();
    }
}

// Store the message in the config struct
void AutoresponderModule::setMessage(const char *message)
{
    if (*message) {
        LOG_DEBUG("Autoresponder: setting message to \"%s\"\n", message);
        strcpy(arConfig.message, message);
        arConfig.bootcount = 0; // Reset the boot count
        saveData<AutoresponderConfig>(&arConfig);

        char feedback[200];
        sprintf(feedback, "Message set to \"%s\"\n", message);
        sendPhoneFeedback(feedback);
    }
}

// Arrives as raw string. Processed into NodeNums, then stored in config struct
void AutoresponderModule::setPermittedNodes(const char *rawString)
{
    char nodeIDBuilder[9]{};
    nodeIDBuilder[8] = '\0';            // Pre-set the null term.
    arConfig.permitted_nodes_count = 0; // Invalidate the previous list of nodes
    uint8_t n = 0;                      // Iterator for NodeID builder
    uint8_t r = 0;                      // Iterator for rawString
    uint8_t p = 0;                      // Iterator for the permitted nodes list (in protobuf)
    LOG_DEBUG("Autoresponder: parsing NodeIDs ");
    do {
        // Grab the next character from the raw list
        static char c;
        c = tolower(rawString[r]); // hex-strings to lower case

        // If char is 0-9 or a-f
        if (isdigit(c) || (c >= 'a' && c <= 'f')) {
            // Add this character to the builder
            nodeIDBuilder[n] = c;
            n++;

            // If we've got enough hex-chars for a 32bit number
            if (n == 8) {
                arConfig.permitted_nodes[p] = (uint32_t)std::stoul(nodeIDBuilder, nullptr, 16); // Parse hex string
                LOG_DEBUG("%s=%zu,", nodeIDBuilder, arConfig.permitted_nodes[p]);               // Log decoded value
                // Increment counters (for array in protobufs)
                arConfig.permitted_nodes_count++;
                p++;
                // Reset iterator (NodeID building)
                n = 0;
            }
        }

        // If pemitted node list full, break the loop
        if (p > (sizeof(arConfig.permitted_nodes) / sizeof(arConfig.permitted_nodes[0])))
            break;

        // Increment (raw string input)
        r++;
    } while (r < strlen(rawString)); // Stop if we run out of raw string input
    LOG_DEBUG("\n");                 // Close this log line

    arConfig.bootcount = 0; // Reset the boot count
    saveData<AutoresponderConfig>(&arConfig);

    // If node list emptied, exit now
    if (arConfig.permitted_nodes_count == 0) {
        sendPhoneFeedback("Will respond to any node");
        return;
    }

    // Convert each permitted NodeNum to a hex string, for phone feedback
    char feedback[200] = {0};

    strcat(feedback, "Permitted nodes are ");

    for (uint8_t i = 0; i < arConfig.permitted_nodes_count; i++) {
        char nodeId[10];
        sprintf(nodeId, "!%0x", arConfig.permitted_nodes[i]);
        strcat(feedback, nodeId);

        // Append a delimiter, if needed
        if (i < arConfig.permitted_nodes_count - 1)
            strcat(feedback, ", ");
    }

    // Send the list of nodes back to the phone
    sendPhoneFeedback(feedback);
}

// Enable / disable both channel and DM together, save settings
void AutoresponderModule::setEnabled(bool enabled)
{
    sendPhoneFeedback(enabled ? "Enabled for DMs and in channel" : "Fully disabled");
    arConfig.enabled_dm = enabled;
    arConfig.enabled_in_channel = enabled;
    saveData<AutoresponderConfig>(&arConfig);
    if (enabled)
        reboot();
}

// Enable / disable DM response, save settings
void AutoresponderModule::setEnabledDM(bool enabled)
{
    sendPhoneFeedback(enabled ? "Enabling for DM" : "Disabling for DM");
    arConfig.enabled_dm = enabled;
    saveData<AutoresponderConfig>(&arConfig);
    reboot();
}

// Enable / disable in channel response, save settings
void AutoresponderModule::setEnabledChannel(bool enabled)
{
    sendPhoneFeedback(enabled ? "Enabling in channel" : "Disabling in channel");
    arConfig.enabled_in_channel = enabled;
    saveData<AutoresponderConfig>(&arConfig);
    reboot();
}

// Set how often a repeated response to the same node is permitted
void AutoresponderModule::setRepeatHours(uint32_t hours)
{
    char feedback[50];
    sprintf(feedback, "Allowing repeat responses every %zu hours", hours);
    sendPhoneFeedback(feedback);
    arConfig.repeat_hours = hours;
    saveData<AutoresponderConfig>(&arConfig);
    reboot();
}

// Set how long until responses are auto-disabled
void AutoresponderModule::setExpirationHours(uint32_t hours)
{
    // Construct a monstrous heads-up message to user
    bool tooLong = hours > maxExpirationChannelHours;
    char lengthWarning[50];
    sprintf(lengthWarning, "Max. timeout for channel is %zu hours.", maxExpirationChannelHours);

    bool forDM = arConfig.should_dm_expire && arConfig.enabled_dm;
    bool forChannel = arConfig.enabled_in_channel;

    char feedback[200] = "";
    sprintf(feedback, "Responses will disable after %zu hours. %s%s%s", hours, forChannel ? "Affects channel." : "",
            forDM ? "Affects DMs. " : "", tooLong ? lengthWarning : "");

    sendPhoneFeedback(feedback);
    arConfig.expiration_hours = hours;
    saveData<AutoresponderConfig>(&arConfig);
    reboot();
}

// Set whether DM should auto-disable along with channel responses
void AutoresponderModule::setShouldDmExpire(bool shouldExpire)
{
    char feedback[50];
    if (enabled)
        sprintf(feedback, "Will stop responding to DMs in %zu hours", arConfig.expiration_hours);
    else
        sprintf(feedback, "Will respond to DMs indefinitely");
    sendPhoneFeedback(feedback);
    arConfig.should_dm_expire = shouldExpire;
    saveData<AutoresponderConfig>(&arConfig);
    reboot();
}

// Send a list of commands to the phone
void AutoresponderModule::printHelp()
{
#define helpCmd(a, b) a " - " b "\n"
#define helpSet(a, b) "    " a " " b "\n"
#define helpOpt(a) "    " a "\n"
    sendPhoneFeedback("You can:\n");
    sendPhoneFeedback(helpCmd("set", "change a setting") helpSet("message", "<text>") helpSet("permitted_nodes", "<NodeIDs/all>")
                          helpSet("repeat_hours", "<number>") helpSet("expiration_hours", "<number>")
                              helpSet("should_dm_expire", "<true/false>"));
    sendPhoneFeedback(helpCmd("enable", "begin responding") helpOpt("(everywhere)") helpOpt("dm") helpOpt("channel"));
    sendPhoneFeedback(helpCmd("disable", "stop responding") helpOpt("(everywhere)") helpOpt("dm") helpOpt("channel"));
#undef helpCmd
#undef helpSet
#undef helpOpt
}

// A DM arrived from the mesh. Maybe send an autoresponse?
void AutoresponderModule::handleDM(const meshtastic_MeshPacket &mp)
{
    // Abort if not enabled for DMs
    if (!arConfig.enabled_dm)
        return;

    // ABORT if the message was from our node
    if (!mp.from)
        return;

    // Abort if we already responded to this node
    if (heardInDM.find(mp.from) != heardInDM.end()) { // (Is NodeNum in the set?)
        LOG_DEBUG("Autoresponder: ignoring DM. Already responded to this node\n");
        return;
    }

    // Abort if "permitted nodes" list used, and sender not found
    if (!isNodePermitted(mp.from)) {
        LOG_DEBUG("Autoresponder: ignoring DM. Sender not found in list of permitted nodes\n");
        return;
    }

    // Send the auto-response, mark that we're waiting for an ACK
    LOG_DEBUG("Autoresponder: responding to a message via DM\n");
    sendText(mp.from, mp.channel, arConfig.message, true);
    respondingTo = mp.from; // Record the original sender
    waitingForAck = true;
    wasDM = true; // Indicate that a successful ACK should add this user to the heardInDM set
}

// A message arrived from a mesh channel. Maybe send a response?
void AutoresponderModule::handleChannel(const meshtastic_MeshPacket &mp)
{
    // ABORT if in-channel response is disabled
    if (!arConfig.enabled_in_channel)
        return;

    // ABORT if not primary channnel
    if (mp.channel != 0)
        return;

    // ABORT if the message was from our node
    if (!mp.from)
        return;

    // ABORT if too many responses in channel within past 24 hours
    if (responsesInChannelToday > maxResponsesChannelDaily) {
        LOG_DEBUG("Autoresponder: too many responses sent in-channel within last 24 hours\n");
        return;
    }

    uint32_t now = millis();

    // ABORT
    if (now - prevInChannelResponseMs < (cooldownChannelMinutes * MS_IN_MINUTE) && prevInChannelResponseMs != 0) {
        LOG_DEBUG("Autoresponder: cooldown (in-channel). No responses to anyone right now.\n");
        return;
    }

    // ABORT if we already responded to this node
    if (heardInChannel.find(mp.from) != heardInChannel.end()) { // (Is NodeNum in set?)
        LOG_INFO("Autoresponder: ignoring channel message, already responded to this node\n");
        return;
    }

    // ABORT if "permitted nodes" list used, and sender not found
    if (!isNodePermitted(mp.from)) {
        LOG_INFO("Autoresponder: ignoring channel message, sender not found in list of permitted nodes\n");
        return;
    }

    // If channel changed (without a reboot), reset the timer and clear the list of seen nodes
    char *currentChannelName = channels.getByIndex(0).settings.name;
    if (strcmp(currentChannelName, channelName) != 0) {
        LOG_DEBUG("Autoresponder: detected a channel change\n");
        clearHeardInChannel();
        strcpy(channelName, currentChannelName);
    }

    // Send the auto-response, then mark that we're waiting for an ACK
    LOG_DEBUG("Autoresponder: responding to a message in channel\n");
    sendText(NODENUM_BROADCAST, 0, arConfig.message, true); // Respond on primary channel
    respondingTo = mp.from;                                 // Record the original sender
    responsesInChannelToday++;                              // Increment "overall" in-channel message count
    prevInChannelResponseMs = now;                          // Record time for "overall" in-channel rate limit
    waitingForAck = true;                                   // Start listening for an ACK
    wasDM = false; // Indicate that a successful ACK should add this user to the heardInChannel list
}

// If we send an autoresponse, this method listens for a relevant ack, before marking the node as "responded to"
void AutoresponderModule::checkForAck(const meshtastic_MeshPacket &mp)
{
    // The payload portion of the mesh packet
    const meshtastic_Data &p = mp.decoded;

    // Decode the routing packet from the original payload
    meshtastic_Routing rp = meshtastic_Routing_init_default;
    pb_decode_from_bytes(p.payload.bytes, p.payload.size, meshtastic_Routing_fields, &rp);

    // If packet was an ACK for our outgoing message
    if (rp.error_reason == meshtastic_Routing_Error_NONE && p.request_id == outgoingId) {
        LOG_DEBUG("Autoresponder: got an ACK for latest message\n");
        waitingForAck = false;

        // Mark that the node saw our message
        if (wasDM) {
            heardInDM.emplace(respondingTo);
            LOG_DEBUG("Autoresponder: adding %zu to heardInDM set\n", respondingTo);
        } else {
            heardInChannel.emplace(respondingTo); // No way of knowing exactly who heard us in channel..
            LOG_DEBUG("Autoresponder: adding %zu to heardInChannel set\n", respondingTo);
        }
    }
}

// Send a text message over the mesh. "Borrowed" from canned message module
void AutoresponderModule::sendText(NodeNum dest, ChannelIndex channel, const char *message, bool wantReplies)
{
    meshtastic_MeshPacket *p = router->allocForSending();
    p->to = dest;
    p->channel = channel;
    p->want_ack = true;
    p->decoded.payload.size = strlen(message);
    p->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
    memcpy(p->decoded.payload.bytes, message, p->decoded.payload.size);

    LOG_DEBUG("Sending message id=%d, dest=%x, msg=%.*s\n", p->id, p->to, p->decoded.payload.size, p->decoded.payload.bytes);

    service.sendToMesh(p, RX_SRC_LOCAL, true);

    // Store the ID and source of this packet, to check for the ACK later
    outgoingId = p->id;
}

// Is this node in the list of "permitted nodes"?
bool AutoresponderModule::isNodePermitted(NodeNum node)
{
    // If list empty, all nodes allowed
    if (arConfig.permitted_nodes_count == 0)
        return true;

    // Check to see if node argument is in permitted_nodes[]
    for (pb_size_t i = 0; i < arConfig.permitted_nodes_count; i++) {
        if (arConfig.permitted_nodes[i] == node)
            return true;
    }

    // Not found in permitted_nodes[]
    return false;
}

// Is the primary channel public (longfast)
bool AutoresponderModule::isPrimaryPublic()
{
    return (strcmp(channels.getByIndex(0).settings.name, "") == 0); // If name is empty
}

// Anti-flooding feature: track how many times the device has rebooted,
// disable response once limit reached
void AutoresponderModule::bootCounting()
{
    // ABORT: if no need to count boots currently
    if (!arConfig.enabled_in_channel && (!arConfig.enabled_dm || !arConfig.should_dm_expire))
        return;

    uint32_t &bootcount = arConfig.bootcount; // Shortcut for annoyingly long setting

    // Not disabled yet, just log the current count
    if (bootcount < expireAfterBootNum) {
        bootcount++;
        LOG_DEBUG("Autoresponder: Boot number %zu of %zu before autoresponse is disabled. (in channel", bootcount,
                  expireAfterBootNum);
        if (arConfig.enabled_dm && arConfig.should_dm_expire && arConfig.expiration_hours)
            LOG_DEBUG(" and for DMs");
        LOG_DEBUG(")\n");
        saveData<AutoresponderConfig>(&arConfig);
    }
    // Disable if too many boots
    else {
        // This only runs once, because this block cannot be reached once in-channel is disabled
        LOG_WARN("Autoresponder: Booted %zu times since module enabled. Disabling response to prevent "
                 "mesh flooding.\n",
                 bootcount);
        arConfig.enabled_in_channel = false;
        if (arConfig.should_dm_expire)
            arConfig.enabled_dm = false;
        bootcount = 0;
        saveData<AutoresponderConfig>(&arConfig); // Save boot count
    }
}

// Restart the device (after applying certain settings)
void AutoresponderModule::reboot()
{
    if (screen)
        screen->startRebootScreen();
    rebootAtMsec = millis() + DEFAULT_REBOOT_SECONDS * 1000;
}

// Runs peridiocally. Scheduled tasks are handled here
int32_t AutoresponderModule::runOnce()
{
    // Periodic tasks
    static uint32_t prevClearDM = 0;
    static uint32_t prevClearChannel = 0;
    static uint32_t prevDailyTasks = 0;

    // Determine intervals
    uint32_t intervalClearDM = MS_IN_MINUTE * repeatDMMinutes;
    uint32_t intervalDailyTasks = MS_IN_MINUTE * 24;
    uint32_t intervalClearChannel =
        MS_IN_MINUTE * max(arConfig.repeat_hours, (isPrimaryPublic() ? minRepeatPubChanHours : minRepeatPrivChanHours));

    uint32_t now = millis();

    // I think millis overflow should take care of itself..(?)

    // ----- Periodic Task -----
    // Clear the heardInDM set, allow repeated responses
    if (arConfig.enabled_dm) {
        if (now - prevClearDM > intervalClearDM) {
            prevClearDM = now;
            clearHeardInDM();
        }
    }

    // ----- Periodic Task -----
    // Clear the heardInChannel set, allow repeated responses
    if (arConfig.enabled_in_channel) {
        if (now - prevClearChannel > intervalClearChannel) {
            prevClearChannel = now;
            clearHeardInChannel();
        }
    }

    // ----- Periodic Task -----
    // Reset daily limits
    if (now - prevDailyTasks > intervalDailyTasks) {
        prevDailyTasks = now;
        handleDailyTasks();
    }

    // ----- Single-shot Task -----
    // Disable in-channel response (time limit)
    if (arConfig.enabled_in_channel) {

        // Determing whether user's expiration value is acceptable
        uint32_t expirationHours;
        if (arConfig.expiration_hours > 0 && arConfig.expiration_hours < maxExpirationChannelHours)
            expirationHours = arConfig.expiration_hours;
        else
            expirationHours = maxExpirationChannelHours;

        // Check if task is due
        if (now > expirationHours * MS_IN_MINUTE)
            handleExpiredChannel();
    }

    // ----- Single-shot Task -----
    // Disable DM response (time limit, optional)
    if (arConfig.enabled_dm && arConfig.should_dm_expire && arConfig.expiration_hours > 0) {
        if (now > arConfig.expiration_hours * MS_IN_MINUTE)
            handleExpiredDM();
    }

    // Run thread every minute
    return 60 * 1000UL;
}

// Clear the collection of nodes we have already heard (via DM). Allows repeat messages
void AutoresponderModule::clearHeardInDM()
{
    heardInDM.clear();
    LOG_INFO("Cleared list of nodes heard via DM\n");
}

// Clear the collection of nodes we have already heard (via channel). Allows repeat messages
void AutoresponderModule::clearHeardInChannel()
{
    heardInChannel.clear();
    LOG_INFO("Cleared list of nodes heard in channel\n");
}

// Handle any tasks which should run daily (clear daily limits)
void AutoresponderModule::handleDailyTasks()
{
    // Reset the total daily limit for in-channel messages
    responsesInChannelToday = 0;
    LOG_INFO("Resetting daily limits\n");
}

// Disable in-channel responses, when expiry time is reached
void AutoresponderModule::handleExpiredChannel()
{
    LOG_INFO("In-channel responses disabled, expiry time reached.\n");
    arConfig.enabled_in_channel = false;
    arConfig.bootcount = 0;
    saveData<AutoresponderConfig>(&arConfig);
}

// Disables DM responses, if DMs responses are set to expire
void AutoresponderModule::handleExpiredDM()
{
    LOG_INFO("DM responses disabled, expiry time reached.\n");
    arConfig.enabled_dm = false;
    arConfig.bootcount = 0;
    saveData<AutoresponderConfig>(&arConfig);
}

#endif