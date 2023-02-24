#ifndef EVENTRECORDER_HPP_
#define EVENTRECORDER_HPP_

#include <dpp/dpp.h>

#include <list>

class eventRecorder
{
public:

  eventRecorder() = default;

  ~eventRecorder() = default;

  void addRecord(const dpp::message_create_t);

  const dpp::message_create_t getRecord(int) const;

  const int maxRecords = 100;

private:

  std::list<dpp::message_create_t> eventRecords;

};

#endif /* EVENTRECORDER_HPP_ */
