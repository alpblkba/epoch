#include "epoch/types.h"

enum epoch_endpoint epoch_msg_endpoint(enum epoch_msg_type type)
{
    /* v0.5 defines no EP2 message yet, so EP2 has no member here. */
    switch (type) {
    case EPOCH_MSG_INCREMENT:
    case EPOCH_MSG_EP1_REPLY:
        return EPOCH_EP1;
    default:
        return EPOCH_EP0;
    }
}

const char *epoch_msg_name(enum epoch_msg_type type)
{
    switch (type) {
    case EPOCH_MSG_PROBE: return "PROBE";
    case EPOCH_MSG_ANNOUNCE: return "ANNOUNCE";
    case EPOCH_MSG_HELLO: return "HELLO";
    case EPOCH_MSG_HELLO_ACK: return "HELLO_ACK";
    case EPOCH_MSG_START: return "START";
    case EPOCH_MSG_START_ACK: return "START_ACK";
    case EPOCH_MSG_PING: return "PING";
    case EPOCH_MSG_PONG: return "PONG";
    case EPOCH_MSG_NO_SESSION: return "NO_SESSION";
    case EPOCH_MSG_BUSY: return "BUSY";
    case EPOCH_MSG_INCREMENT: return "INCREMENT";
    case EPOCH_MSG_EP1_REPLY: return "EP1_REPLY";
    case EPOCH_MSG_TEST_ARM: return "TEST_ARM";
    case EPOCH_MSG_TEST_ARM_ACK: return "TEST_ARM_ACK";
    case EPOCH_MSG_TEST_DIAG: return "TEST_DIAG";
    case EPOCH_MSG_TEST_DIAG_REPLY: return "TEST_DIAG_REPLY";
    default: return "?";
    }
}

const char *epoch_outcome_name(enum epoch_outcome outcome)
{
    switch (outcome) {
    case EPOCH_OUTCOME_COMPLETED: return "COMPLETED";
    case EPOCH_OUTCOME_REJECTED: return "REJECTED";
    case EPOCH_OUTCOME_NOT_EXECUTED: return "NOT_EXECUTED";
    case EPOCH_OUTCOME_UNKNOWN: return "UNKNOWN";
    default: return "NONE";
    }
}
