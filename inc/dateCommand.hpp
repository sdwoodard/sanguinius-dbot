#ifndef DATECOMMAND_HPP_
#define DATECOMMAND_HPP_

#include <dpp/dpp.h>
#include <ICommand.hpp>

class dateCommand : public ICommand
{
public:

  dateCommand() = default;

  ~dateCommand() = default;

  void registerCommand(std::shared_ptr<dpp::cluster> bot) override;

  void commandCallBack(std::string command, const dpp::slashcommand_t& event) override;

private:

  void executeCommand(const dpp::slashcommand_t& event);

  std::shared_ptr<dpp::cluster> bot;

};

#endif /* DATECOMMAND_HPP_ */
