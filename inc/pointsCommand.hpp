#ifndef POINTSCOMMAND_HPP_
#define POINTSCOMMAND_HPP_

#include <dpp/dpp.h>
#include <ICommand.hpp>
#include <pointHandler.hpp>
#include <boost/log/core.hpp>
#include <boost/log/trivial.hpp>
#include <boost/log/sources/severity_logger.hpp>

class pointsCommand : public ICommand
{
public:

  pointsCommand(pointHandler* acPointHandler, boost::log::sources::severity_logger<boost::log::trivial::severity_level>* apcLogger);

  ~pointsCommand() = default;

  bool commandCallBack(std::string keyword, const dpp::message_create_t& event) override;

private:

  void executeCommand(const dpp::message_create_t& event);

  dpp::cluster* bot;

  pointHandler* mpcPointHandler;

  boost::log::sources::severity_logger<boost::log::trivial::severity_level>* mpcLogger;

};

#endif /* POINTSCOMMAND_HPP_ */
