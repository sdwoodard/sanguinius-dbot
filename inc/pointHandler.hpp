#ifndef POINTHANDLER_HPP_
#define POINTHANDLER_HPP_

#include <map>

#include <dpp/dpp.h>
#include <boost/log/core.hpp>
#include <boost/log/trivial.hpp>
#include <boost/log/sources/severity_logger.hpp>

class pointHandler
{
public:
   pointHandler(boost::log::sources::severity_logger<boost::log::trivial::severity_level>* apcLogger);

   ~pointHandler() = default;

   int getPoints(dpp::snowflake id) const;

   void updatePoints(dpp::snowflake id, int pointsDelta);

private:
   std::map<dpp::snowflake, int> users_;

   boost::log::sources::severity_logger<boost::log::trivial::severity_level>* mpcLogger;
};

#endif /* POINTHANDLER_HPP_ */
