#pragma once
#include "MeshModule.h"
#include "concurrency/OSThread.h"
#include <unordered_set>

class AutoresponderModule : public MeshModule, protected concurrency::OSThread
{
  public:
    AutoresponderModule();

  protected:
    // MeshModule overrides
    virtual bool wantPacket(const meshtastic_MeshPacket *p) override;
    virtual ProcessMessage handleReceived(const meshtastic_MeshPacket &mp) override;
    virtual AdminMessageHandleResult handleAdminMessageForModule(const meshtastic_MeshPacket &mp,
                                                                 meshtastic_AdminMessage *request,
                                                                 meshtastic_AdminMessage *response) override;

    // Interact with mesh data
    void handleDM(const meshtastic_MeshPacket &mp);      // Reply if message meets the criteria for a DM response
    void handleChannel(const meshtastic_MeshPacket &mp); // Reply if message meets the criteria for in-channel response
    void checkForAck(const meshtastic_MeshPacket &mp);   // Check if an outgoing message was received
    void sendText(NodeNum dest, ChannelIndex channel, const char *message, bool wantReplies); // Send a text message over the mesh
    bool isPrimaryPublic();

    // Get and set config
    void loadProtoForModule();
    void saveProtoForModule();
    void setDefaultConfig();
    void handleGetConfigMessage(const meshtastic_MeshPacket &req, meshtastic_AdminMessage *response);
    void handleGetConfigPermittedNodes(const meshtastic_MeshPacket &req, meshtastic_AdminMessage *response);
    void handleSetConfigMessage(const char *message);
    void handleSetConfigPermittedNodes(const char *rawString);
    bool isNodePermitted(NodeNum node);

    // Scheduled tasks
    virtual int32_t runOnce() override; // Runs once per minute. From OSThread class
    void clearHeardInDM();
    void clearHeardInChannel();
    void handleDailyTasks();
    void handleExpiredChannel();
    void handleExpiredDM();

    void bootCounting();

    bool waitingForAck = false; // If true, we temporarily want routing packets, to check for ACKs
    PacketId outgoingId;        // Packet ID of our latest outgoing auto-response, to check for ACK
    NodeNum respondingTo;       // NodeNum dest of latest auto-response, to store if we get an ACK
    bool wasDM;                 // Is our response to a DM, or to a message we heard in channel?

    char channelName[12] = ""; // Detect changes in channel (can happen without reboot)

    std::unordered_set<NodeNum> heardInDM, heardInChannel; // Node numbers which we have responded to
    uint16_t responsesInChannelToday = 0;                  // How many responses have been sent in-channel, within last 24 hours
    uint32_t prevInChannelResponseMs = 0;                  // When was the previous in-channel response sent?
};
