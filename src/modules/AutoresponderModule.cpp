#include "AutoresponderModule.h"
#include "MeshService.h"
#include "configuration.h"
#include "main.h"

#include <assert.h>

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