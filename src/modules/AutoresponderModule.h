#pragma once
#include "SinglePortModule.h"

class AutoresponderModule : public SinglePortModule
{
  public:
    AutoresponderModule() : SinglePortModule("autoresponder", meshtastic_PortNum_AUTORESPONDER_APP) {}

  protected:
    virtual bool wantPacket(const meshtastic_MeshPacket *p) override;
    virtual ProcessMessage handleReceived(const meshtastic_MeshPacket &mp) override;

    void checkIfDM(const meshtastic_MeshPacket &mp);   // Check if message was a DM for us, then reply if needed
    void checkForAck(const meshtastic_MeshPacket &mp); // Check if an outgoing message was received

    void sendText(NodeNum dest, ChannelIndex channel, const char *message, bool wantReplies); // Send a text message over the mesh

    bool waitingForAck = false; // If true, we temporarily want routing packets, to check for ACKs
    PacketId outgoingId;        // ID of our latest outgoing auto-responce, to check for ACK
};
