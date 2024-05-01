#pragma once
#include "SinglePortModule.h"
#include <unordered_set>

class AutoresponderModule : public SinglePortModule
{
  public:
    AutoresponderModule();

  protected:
    virtual bool wantPacket(const meshtastic_MeshPacket *p) override;
    virtual ProcessMessage handleReceived(const meshtastic_MeshPacket &mp) override;
    virtual AdminMessageHandleResult handleAdminMessageForModule(const meshtastic_MeshPacket &mp,
                                                                 meshtastic_AdminMessage *request,
                                                                 meshtastic_AdminMessage *response) override;

    void handleDM(const meshtastic_MeshPacket &mp);      // Reply if message meets the criteria for a DM response
    void handleChannel(const meshtastic_MeshPacket &mp); // Reply if message meets the criteria for in-channel response
    void checkForAck(const meshtastic_MeshPacket &mp);   // Check if an outgoing message was received

    void sendText(NodeNum dest, ChannelIndex channel, const char *message, bool wantReplies); // Send a text message over the mesh

    void loadProtoForModule();
    void saveProtoForModule();
    void setDefaultConfig();
    void handleGetConfigMessage(const meshtastic_MeshPacket &req, meshtastic_AdminMessage *response);
    void handleGetConfigPermittedNodes(const meshtastic_MeshPacket &req, meshtastic_AdminMessage *response);
    void handleSetConfigMessage(const char *message);
    void handleSetConfigPermittedNodes(const char *rawString);

    bool isNodePermitted(NodeNum node);
    void handleDayRollover();

    bool waitingForAck = false; // If true, we temporarily want routing packets, to check for ACKs
    PacketId outgoingId;        // Packet ID of our latest outgoing auto-response, to check for ACK
    NodeNum respondingTo;       // NodeNum dest of latest auto-response, to store if we get an ACK
    bool wasDM;                 // Is our response to a DM, or to a message we heard in channel?

    std::unordered_set<NodeNum> heardDM, heardInChannel; // Node numbers which have DM'd us

    uint32_t lastInChannelMs = 0;
    uint32_t runningSinceMS = 0;
    uint8_t inChannelResponseCount = 0;
};
