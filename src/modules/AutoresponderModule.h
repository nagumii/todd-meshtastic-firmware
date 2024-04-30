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

    void checkIfDM(const meshtastic_MeshPacket &mp);   // Check if message was a DM for us, then reply if needed
    void checkForAck(const meshtastic_MeshPacket &mp); // Check if an outgoing message was received

    void sendText(NodeNum dest, ChannelIndex channel, const char *message, bool wantReplies); // Send a text message over the mesh

    void loadProtoForModule();
    void saveProtoForModule();
    void setDefaultConfig();

    void handleGetConfigMessage(const meshtastic_MeshPacket &req, meshtastic_AdminMessage *response);
    void handleGetConfigPermittedNodes(const meshtastic_MeshPacket &req, meshtastic_AdminMessage *response);
    void handleSetConfigMessage(const char *message);
    void handleSetConfigPermittedNodes(const char *rawString);

    bool isNodePermitted(NodeNum node);

    bool waitingForAck = false;             // If true, we temporarily want routing packets, to check for ACKs
    PacketId outgoingId;                    // ID of our latest outgoing auto-responce, to check for ACK
    std::unordered_set<uint32_t> heardFrom; // Node numbers which have DM'd us
};
