#ifndef ROLLCOMMAND_HPP_
#define ROLLCOMMAND_HPP_

#include <dpp/dpp.h>
#include <ICommand.hpp>

class rollCommand : public ICommand
{
public:

  rollCommand() = default;

  ~rollCommand() = default;

  bool commandCallBack(std::string keyword, const dpp::message_create_t& event) override;

private:

  void executeCommand(const dpp::message_create_t& event);

  std::shared_ptr<dpp::cluster> bot;

};

#endif /* ROLLCOMMAND_HPP_ */
