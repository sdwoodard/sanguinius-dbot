#include <pointHandler.hpp>

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <map>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <dpp/dpp.h>

pointHandler::pointHandler(boost::log::sources::severity_logger<boost::log::trivial::severity_level>* apcLogger)
: mpcLogger(apcLogger)
{
   BOOST_LOG_SEV(*apcLogger, boost::log::trivial::info) << "Entering pointHandler constructor"; boost::log::core::get()->flush();
   // read XML file and populate map
   std::string filename = "/home/sigmar/git/sanguinius-dbot/data/userPoints.xml";
   std::ifstream file(filename);
   if (!file.good()) {
      std::cerr << "Error opening file: " << filename << std::endl;
      return;
   }
   BOOST_LOG_SEV(*apcLogger, boost::log::trivial::info) << "Reading file: " << filename; boost::log::core::get()->flush();

   boost::property_tree::ptree tree;
   boost::property_tree::read_xml(file, tree);

   BOOST_LOG_SEV(*apcLogger, boost::log::trivial::info) << "Populating map"; boost::log::core::get()->flush();

   for (auto& child : tree.get_child("users")) {
      dpp::snowflake id = child.second.get<dpp::snowflake>("id");
      int points = child.second.get<int>("points");
      users_[id] = points;
   }

   BOOST_LOG_SEV(*apcLogger, boost::log::trivial::info) << "Leaving pointHandler constructor"; boost::log::core::get()->flush();

   file.close();
   BOOST_LOG_SEV(*apcLogger, boost::log::trivial::info) << "File closed"; boost::log::core::get()->flush();
}

int pointHandler::getPoints(dpp::snowflake id) const {
   BOOST_LOG_SEV(*mpcLogger, boost::log::trivial::info) << "Entering getPoints"; boost::log::core::get()->flush();
   auto it = users_.find(id);
   BOOST_LOG_SEV(*mpcLogger, boost::log::trivial::info) << "Looking up user with ID: " << id; boost::log::core::get()->flush();
   if (it == users_.end()) {
      BOOST_LOG_SEV(*mpcLogger, boost::log::trivial::error) << "User with ID " << id << " not found"; boost::log::core::get()->flush();
      return -1;
   }
   BOOST_LOG_SEV(*mpcLogger, boost::log::trivial::info) << "Returning points: " << it->second; boost::log::core::get()->flush();
   return it->second;
}

void pointHandler::updatePoints(dpp::snowflake id, int pointsDelta) {
   auto it = users_.find(id);
   if (it == users_.end()) {
      std::cerr << "Error: user with ID " << id << " not found." << std::endl;
      return;
   }
   int newPoints = it->second + pointsDelta;
   it->second = newPoints;

   // update XML file
   boost::property_tree::ptree tree;
   boost::property_tree::read_xml("/home/sigmar/git/sanguinius-dbot/data/userPoints.xml", tree);

   for (auto& child : tree.get_child("users")) {
      if (child.second.get<dpp::snowflake>("id") == id) {
         child.second.put("points", newPoints);
         break;
      }
   }

   boost::property_tree::write_xml("/home/sigmar/git/sanguinius-dbot/data/userPoints.xml", tree);
}
