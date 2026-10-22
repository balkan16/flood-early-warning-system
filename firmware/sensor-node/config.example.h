// Copy this file to config.h and fill in your values.
// config.h is listed in .gitignore so real phone numbers never reach the repo.
#pragma once

// Primary number = the tower receiver node's SIM.
// Receives every status change and the daily self-test report (used as heartbeat).
#define PRIMARY_NUMBER   "+62XXXXXXXXXX"
// Secondary number = a person's phone (backup alert).
// Receives only NORMAL, SIAGA 1 and SIAGA 2.
#define SECONDARY_NUMBER "+62XXXXXXXXXX"
