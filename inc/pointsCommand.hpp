#ifndef POINTSCOMMAND_HPP_
#define POINTSCOMMAND_HPP_

#include <dpp/dpp.h>
#include <ICommand.hpp>
#include <pointHandler.hpp>

class pointsCommand : public ICommand
{
public:

  pointsCommand(pointHandler* acPointHandler);

  ~pointsCommand() = default;

  bool commandCallBack(std::string keyword, const dpp::message_create_t& event) override;

private:

  void executeCommand(const dpp::message_create_t& event);

  dpp::cluster* bot;

  pointHandler* mpcPointHandler;

};

#endif /* POINTSCOMMAND_HPP_ */
