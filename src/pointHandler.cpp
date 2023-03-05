#include <pointHandler.hpp>

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <map>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <dpp/dpp.h>

pointHandler::pointHandler() {
   // read XML file and populate map
   std::string filename = "/home/sigmar/git/sanguinius-dbot/data/userPoints.xml";
   std::ifstream file(filename);
   if (!file.good()) {
      std::cerr << "Error opening file: " << filename << std::endl;
      return;
   }

   boost::property_tree::ptree tree;
   boost::property_tree::read_xml(file, tree);

   for (auto& child : tree.get_child("users")) {
      dpp::snowflake id = child.second.get<dpp::snowflake>("id");
      int points = child.second.get<int>("points");
      users_[id] = points;
   }

   file.close();
}

int pointHandler::getPoints(dpp::snowflake id) const {
   auto it = users_.find(id);
   if (it == users_.end()) {
      std::cerr << "Error: user with ID " << id << " not found." << std::endl;
      return -1;
   }
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
