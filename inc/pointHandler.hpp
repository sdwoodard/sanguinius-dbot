#ifndef POINTHANDLER_HPP_
#define POINTHANDLER_HPP_

#include <map>

#include <dpp/dpp.h>

class pointHandler
{
public:
   pointHandler();

   ~pointHandler() = default;

   int getPoints(dpp::snowflake id) const;

   void updatePoints(dpp::snowflake id, int pointsDelta);

private:
   std::map<dpp::snowflake, int> users_;
};

#endif /* POINTHANDLER_HPP_ */
