#include "AutoresponderModule.h"
#include "MeshService.h"
#include "configuration.h"
#include "mesh/generated/meshtastic/autoresponder.pb.h"

#include <assert.h>

static constexpr uint32_t maxInChannelRuntimeMs = 72 * 24 * 60 * 1000UL; // How long before module auto-disables?
static constexpr uint32_t maxInChannelMs = 10 * 60 * 1000UL;             // Minimum interval between in-channel responses
static constexpr uint8_t maxResponsesInChannelDaily = 10;                // Max responses per day, in-channel

// The separate config file for this module (stores strings)
static const char *autoresponderConfigFile = "/prefs/autoresponderConf.proto"; // Location of the file
static meshtastic_AutoresponderConfig autoresponderConfig;                     // Holds config file during runtime

// Constructor
AutoresponderModule::AutoresponderModule() : SinglePortModule("autoresponder", meshtastic_PortNum_AUTORESPONDER_APP)
{
    if (moduleConfig.autoresponder.enabled_dm || moduleConfig.autoresponder.enabled_in_channel) {
        loadProtoForModule();

        // Debug output at boot
        LOG_INFO("Autoresponder module enabled\n");
        if (autoresponderConfig.permitted_nodes_count > 0) {
            LOG_DEBUG("Autoresponder: only responding to node ID ");
            for (uint8_t i = 0; i < autoresponderConfig.permitted_nodes_count; i++) {
                LOG_DEBUG("!%0x", autoresponderConfig.permitted_nodes[i]);
                if (i < autoresponderConfig.permitted_nodes_count - 1)
                    LOG_DEBUG(", ");
            }
            LOG_DEBUG("\n");
        }
    } else
        LOG_INFO("Autoresponder module disabled\n");
}

// Do we want to process this packet with handleReceived()?
bool AutoresponderModule::wantPacket(const meshtastic_MeshPacket *p)
{
    // If module is disabled for both DM and in channel, ignore packets
    if (!moduleConfig.autoresponder.enabled_dm && !moduleConfig.autoresponder.enabled_in_channel)
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

// Check the content of the text message, then take action if required
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

// Handle admin messages (getting and setting config our separate settings file)
AdminMessageHandleResult AutoresponderModule::handleAdminMessageForModule(const meshtastic_MeshPacket &mp,
                                                                          meshtastic_AdminMessage *request,
                                                                          meshtastic_AdminMessage *response)
{
    AdminMessageHandleResult result;

    switch (request->which_payload_variant) {
    case meshtastic_AdminMessage_get_autoresponder_message_request_tag:
        LOG_DEBUG("Client is getting the autoresponder message\n");
        this->handleGetConfigMessage(mp, response);
        result = AdminMessageHandleResult::HANDLED_WITH_RESPONSE;
        break;

    case meshtastic_AdminMessage_set_autoresponder_message_tag:
        LOG_DEBUG("Client is setting the autoresponder message\n");
        this->handleSetConfigMessage(request->set_autoresponder_message);
        result = AdminMessageHandleResult::HANDLED;
        break;

    case meshtastic_AdminMessage_get_autoresponder_permittednodes_request_tag:
        LOG_DEBUG("Client is getting the autoresponder \"permitted nodes\" list\n");
        this->handleGetConfigPermittedNodes(mp, response);
        result = AdminMessageHandleResult::HANDLED_WITH_RESPONSE;
        break;

    case meshtastic_AdminMessage_set_autoresponder_permittednodes_tag:
        LOG_DEBUG("Client is setting the autoresponder \"permitted nodes\" list\n");
        this->handleSetConfigPermittedNodes(request->set_autoresponder_permittednodes);
        result = AdminMessageHandleResult::HANDLED;
        break;

    default:
        result = AdminMessageHandleResult::NOT_HANDLED;
    }

    return result;
}

void AutoresponderModule::handleGetConfigMessage(const meshtastic_MeshPacket &req, meshtastic_AdminMessage *response)
{
    if (req.decoded.want_response) {
        // Mark that response packet contains the current message
        response->which_payload_variant = meshtastic_AdminMessage_get_autoresponder_message_response_tag;
        // Copy the current message into the response packet
        strncpy(response->get_autoresponder_message_response, autoresponderConfig.response_text,
                sizeof(response->get_autoresponder_message_response));
    }
}

void AutoresponderModule::handleGetConfigPermittedNodes(const meshtastic_MeshPacket &req, meshtastic_AdminMessage *response)
{
    if (req.decoded.want_response) {
        // Mark that this response packet contains the list of permitted nodes (as a string representation)
        response->which_payload_variant = meshtastic_AdminMessage_get_autoresponder_permittednodes_response_tag;

        // Convert each permitted NodeNum to a hex string, and add to the response packet
        strcpy(response->get_autoresponder_permittednodes_response, ""); // Start with an empty string

        for (uint8_t i = 0; i < autoresponderConfig.permitted_nodes_count; i++) {
            char nodeId[10];
            sprintf(nodeId, "!%0x", autoresponderConfig.permitted_nodes[i]);
            strcat(response->get_autoresponder_permittednodes_response, nodeId);

            // Append a delimiter, if needed
            if (i < autoresponderConfig.permitted_nodes_count - 1)
                strcat(response->get_autoresponder_permittednodes_response, ", ");
        }
    }
}

void AutoresponderModule::handleSetConfigMessage(const char *message)
{
    if (*message) {
        strncpy(autoresponderConfig.response_text, message, sizeof(autoresponderConfig.response_text));
        this->saveProtoForModule();
        LOG_DEBUG("Autoresponder: setting message to \"%s\"\n", message);
    }
}

// Set the list of "permitted nodes". Decode the raw string into NodeNums, store in protobuf
void AutoresponderModule::handleSetConfigPermittedNodes(const char *rawString)
{
    char nodeIDBuilder[9]{};
    nodeIDBuilder[8] = '\0';                       // Pre-set the null term.
    autoresponderConfig.permitted_nodes_count = 0; // Invalidate the previous list of nodes
    uint8_t n = 0;                                 // Iterator for NodeID builder
    uint8_t r = 0;                                 // Iterator for rawString
    uint8_t p = 0;                                 // Iterator for the permitted nodes list (in protobuf)
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
                autoresponderConfig.permitted_nodes[p] = (uint32_t)std::stoul(nodeIDBuilder, nullptr, 16); // Parse hex string
                LOG_DEBUG("%s=%zu,", nodeIDBuilder, autoresponderConfig.permitted_nodes[p]);               // Log decoded value
                // Increment counters (for array in protobufs)
                autoresponderConfig.permitted_nodes_count++;
                p++;
                // Reset iterator (NodeID building)
                n = 0;
            }
        }

        // Increment (raw string input)
        r++;
    } while (r < strlen(rawString)); // Stop if we run out of raw string input
    LOG_DEBUG("\n");                 // Close this log line

    this->saveProtoForModule();
}

// Reply if message meets the criteria for a DM response
void AutoresponderModule::handleDM(const meshtastic_MeshPacket &mp)
{
    // Abort if not enabled for DMs
    if (!moduleConfig.autoresponder.enabled_dm)
        return;

    // Abort if we already responded to this node
    if (heardDM.find(mp.from) != heardDM.end()) { // (Is NodeNum in the set?)
        LOG_DEBUG("Autoresponder: ignoring DM. Already responded to this node\n");
        return;
    }

    // Abort if "permitted nodes" list used, and sender not found
    if (!isNodePermitted(mp.from)) {
        LOG_DEBUG("Autoresponder: ignoring DM. Sender not found in list of permitted nodes\n");
        return;
    }

    // Send the auto-response, mark that we're waiting for an ACK
    LOG_DEBUG("Autoresponder: sending a reply\n");
    sendText(mp.from, mp.channel, autoresponderConfig.response_text, true);
    respondingTo = mp.from; // Record the original sender
    waitingForAck = true;
    wasDM = true; // Indicate that a successful ACK should add this user to the heardDM set
}

// Reply if message meets the criteria for in-channel response
void AutoresponderModule::handleChannel(const meshtastic_MeshPacket &mp)
{
    uint32_t now = millis();

    // ABORT if in-channel response is disabled
    if (!moduleConfig.autoresponder.enabled_in_channel)
        return;

    // ABORT if not primary channnel
    if (mp.channel != 0)
        return;

    // ABORT if recently responded in channel
    if (now - lastInChannelMs < maxInChannelMs && lastInChannelMs != 0) {
        LOG_DEBUG("Autoresponder: ignoring channel message, too soon since last response\n");
        return;
    }

    // ABORT if we already responded to this node
    if (heardInChannel.find(mp.from) != heardInChannel.end()) { // (Is NodeNum in set?)
        LOG_DEBUG("Autoresponder: ignoring channel message, already responded to this node\n");
        return;
    }

    // ABORT if "permitted nodes" list used, and sender not found
    if (!isNodePermitted(mp.from)) {
        LOG_DEBUG("Autoresponder: ignoring channel message, sender not found in list of permitted nodes\n");
        return;
    }

    // ABORT & disable in-channel, if runtime limit exceeded
    if (now > maxInChannelRuntimeMs) {
        LOG_DEBUG("Autoresponder: disabling in-channel response; running for too long\n");
        moduleConfig.autoresponder.enabled_in_channel = false;
        return;
    }

    // ABORT if too many in-channel responses today
    handleDayRollover();
    if (inChannelResponseCount > maxResponsesInChannelDaily)
        return;

    // Send the auto-response, mark that we're waiting for an ACK
    LOG_DEBUG("Autoresponder: sending a reply\n");
    sendText(NODENUM_BROADCAST, 0, autoresponderConfig.response_text, true); // Respond on primary channel
    respondingTo = mp.from;                                                  // Record the original sender
    waitingForAck = true;
    wasDM = false; // Indicate that a successful ACK should add this user to the heardInChannel set
    lastInChannelMs = now;
}

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
            heardDM.emplace(respondingTo);
            LOG_DEBUG("Autoresponder: adding %zu to heardDM set\n", respondingTo);
        } else {
            heardInChannel.emplace(respondingTo); // No way of knowing exactly who heard us in channel..
            LOG_DEBUG("Autoresponder: adding %zu to heardInChannel set\n", respondingTo);
        }
    }
}

// Send a text message over the mesh. "Borrowed" from canned message module
void AutoresponderModule::sendText(NodeNum dest, ChannelIndex channel, const char *message, bool wantReplies)
{
    meshtastic_MeshPacket *p = allocDataPacket();
    p->to = dest;
    p->channel = channel;
    p->want_ack = true;
    p->decoded.payload.size = strlen(message);
    p->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
    memcpy(p->decoded.payload.bytes, message, p->decoded.payload.size);

    LOG_INFO("Sending message id=%d, dest=%x, msg=%.*s\n", p->id, p->to, p->decoded.payload.size, p->decoded.payload.bytes);

    service.sendToMesh(p, RX_SRC_LOCAL, true);

    // Store the ID and source of this packet, to check for the ACK later
    outgoingId = p->id;
}

// Load the bulk config (separate file, not config.proto)
void AutoresponderModule::loadProtoForModule()
{
    // Attempt to load the proto file into RAM
    LoadFileResult result;
    result = nodeDB->loadProto(autoresponderConfigFile, meshtastic_AutoresponderConfig_size,
                               sizeof(meshtastic_AutoresponderConfig), &meshtastic_AutoresponderConfig_msg, &autoresponderConfig);

    // If load failed, set default values for the config in RAM
    if (result != LoadFileResult::SUCCESS)
        setDefaultConfig();
}

void AutoresponderModule::setDefaultConfig()
{
    LOG_INFO("%s not loaded. Autoresponder using default config\n", autoresponderConfigFile);
    memset(autoresponderConfig.response_text, 0,
           sizeof(autoresponderConfig.response_text)); // Default response text
    memset(autoresponderConfig.permitted_nodes, 0,
           sizeof(autoresponderConfig.permitted_nodes)); // Empty "permitted nodes" array
}

void AutoresponderModule::saveProtoForModule()
{
    LOG_DEBUG("Autoresponder: saving config\n");

#ifdef FS
    FS.mkdir("/prefs");
#endif

    // Attempt to save the module's separate config file
    if (!nodeDB->saveProto(autoresponderConfigFile, meshtastic_AutoresponderConfig_size, &meshtastic_AutoresponderConfig_msg,
                           &autoresponderConfig))
        LOG_ERROR("Couldn't save %s\n", autoresponderConfigFile);

    return;
}

bool AutoresponderModule::isNodePermitted(NodeNum node)
{
    // If list empty, all nodes allowed
    if (autoresponderConfig.permitted_nodes_count == 0)
        return true;

    // Check to see if node argument is in permitted_nodes[]
    for (pb_size_t i = 0; i < autoresponderConfig.permitted_nodes_count; i++) {
        if (autoresponderConfig.permitted_nodes[i] == node)
            return true;
    }

    // Not found in permitted_nodes[]
    return false;
}

// If the day has rolled over, reset the "in-channel" message limit
void AutoresponderModule::handleDayRollover()
{
    static constexpr uint32_t MS_IN_DAY = 24 * 60 * 1000UL;
    if ((millis() / MS_IN_DAY) != (lastInChannelMs / MS_IN_DAY))
        inChannelResponseCount = 0;
}