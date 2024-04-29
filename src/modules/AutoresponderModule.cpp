#include "AutoresponderModule.h"
#include "MeshService.h"
#include "configuration.h"
#include "mesh/generated/meshtastic/autoresponder.pb.h"

#include <assert.h>

// The separate config file for this module (stores strings)
static const char *autoresponderConfigFile = "/prefs/autoresponderConf.proto"; // Location of the file
meshtastic_AutoresponderConfig autoresponderConfig;                            // Holds config file during runtime

// Constructor
AutoresponderModule::AutoresponderModule() : SinglePortModule("autoresponder", meshtastic_PortNum_AUTORESPONDER_APP)
{
    if (moduleConfig.autoresponder.enabled) {
        LOG_DEBUG("Autoresponder: enabled\n");
        loadProtoForModule();
    } else
        LOG_DEBUG("Autoresponder: not enabled\n");
}

// Do we want to process this packet with handleReceived()?
bool AutoresponderModule::wantPacket(const meshtastic_MeshPacket *p)
{
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
        checkIfDM(mp);
        break;
    case meshtastic_PortNum_ROUTING_APP: // Routing (for ACKs)
        checkForAck(mp);
        break;
    default:
        break;
    }

    return ProcessMessage::CONTINUE; // Let others look at this message also if they want
}

// Check if incoming message is a DM directed at us, then take action
void AutoresponderModule::checkIfDM(const meshtastic_MeshPacket &mp)
{
    // If message was a DM to us
    if (mp.to == myNodeInfo.my_node_num) {
        LOG_DEBUG("Autoresponder: sending a reply\n");
        sendText(mp.from, mp.channel, "Autoresponder sees you!", true);
        waitingForAck = true;
    }

    // If message was *not* for us
    else {
        LOG_DEBUG("Autoresponder: message was not a DM for us. Wanted %zu, but we are %zu\n", mp.to, myNodeInfo.my_node_num);
        return;
    }
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

        // -- Mark the node as having seen our message, in nodedb --
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

    // Store the ID of this packet, to check for the ACK later
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

    // Testing only
    // ======================
    for (pb_size_t i = 0; i < autoresponderConfig.permitted_nodes_count; i++) {
        LOG_DEBUG("permitted_nodes[%hu]=%u\n", i, (unsigned int)autoresponderConfig.permitted_nodes[i]);
    }

    if (isNodePermitted(12345678))
        LOG_DEBUG("permitted 12345678\n");
    else
        LOG_DEBUG("not permitted 12345678\n");

    if (isNodePermitted(12345679))
        LOG_DEBUG("permitted 12345679\n");
    else
        LOG_DEBUG("not permitted 12345679\n");
    // =======================
}

void AutoresponderModule::setDefaultConfig()
{
    LOG_INFO("%s not loaded. Autoresponder using default config\n", autoresponderConfigFile);
    memset(autoresponderConfig.response_text, 0,
           sizeof(autoresponderConfig.response_text)); // Default response text
    memset(autoresponderConfig.permitted_nodes, 0,
           sizeof(autoresponderConfig.permitted_nodes)); // Empty "permitted nodes" array

    // Testing only
    // =============
    autoresponderConfig.permitted_nodes[0] = 12345678;
    autoresponderConfig.permitted_nodes[1] = 99999999;
    autoresponderConfig.permitted_nodes_count = 2;
    saveProtoForModule();
    // =============
}

void AutoresponderModule::saveProtoForModule()
{

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
    for (pb_size_t i = 0; i < autoresponderConfig.permitted_nodes_count; i++) {
        if (autoresponderConfig.permitted_nodes[i] == node)
            return true;
    }

    // Not found in permitted_nodes[]
    return false;
}