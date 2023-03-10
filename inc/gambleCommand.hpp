#ifndef GAMBLECOMMAND_HPP_
#define GAMBLECOMMAND_HPP_

#include <dpp/dpp.h>
#include <ICommand.hpp>
#include <pointHandler.hpp>
#include <boost/log/core.hpp>
#include <boost/log/trivial.hpp>
#include <boost/log/sources/severity_logger.hpp>

#include <unordered_map>
#include <vector>

class gambleCommand : public ICommand
{
public:

  gambleCommand(dpp::cluster* acBot, pointHandler* acPointHandler, boost::log::sources::severity_logger<boost::log::trivial::severity_level>* apcLogger);

  ~gambleCommand() = default;

  bool commandCallBack(std::string keyword, const dpp::message_create_t& event) override;

private:

  void executeCommand(const dpp::message_create_t& event);

  dpp::cluster* bot;

  pointHandler* mpcPointHandler;

  boost::log::sources::severity_logger<boost::log::trivial::severity_level>* mpcLogger;

  std::unordered_map<std::string, std::vector<dpp::snowflake>> voteMap;

  std::string currentStake = "";

  int gambleAmount = 0;

  bool ongoingGamble = false;

};

#endif /* DATECOMMAND_HPP_ */
