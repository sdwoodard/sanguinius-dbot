#include <dpp/dpp.h>
#include <eventRecorder.hpp>

void eventRecorder::addRecord(const dpp::message_create_t eventRecord)
{
   if (eventRecords.size() > maxRecords)
   {
      eventRecords.pop_back();
   }
   eventRecords.push_front(eventRecord);
}

const dpp::message_create_t eventRecorder::getRecord(int index) const
{
   auto iterator = eventRecords.begin();
   std::advance(iterator, index);
   return *iterator;
}