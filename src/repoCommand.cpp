#include <dpp/dpp.h>
#include <repoCommand.hpp>

void repoCommand::registerCommand(std::shared_ptr<dpp::cluster> bot)
{
  dpp::slashcommand repocommand("repo", "Start programming!", bot->me.id);
  bot->global_command_create(repocommand);
}

void repoCommand::commandCallBack(std::string command, const dpp::slashcommand_t& event)
{

  if (command == "repo")
  {
    this->executeCommand(event);
  }

}

void repoCommand::executeCommand(const dpp::slashcommand_t& event)
{

  dpp::message response_message;

  dpp::embed response_embed = dpp::embed().
    set_color(dpp::colors::sti_blue).
    set_title("Source code for Sanguinius!").
    set_url("https://github.com/sdwoodard/sanguinius-dbot").
    set_author("Github", "https://github.com/", "https://github.githubassets.com/images/modules/logos_page/GitHub-Mark.png").
    set_description("Code repository for all things required to build the sanguinius discord bot.").
    set_thumbnail("https://github.githubassets.com/images/modules/logos_page/GitHub-Mark.png").
    add_field("Current regular supports:", "Stephen Woodard").
    set_image("https://github.githubassets.com/images/modules/logos_page/GitHub-Mark.png").
    set_footer(dpp::embed_footer().set_text("Come by sometime and coding yourself!")).
    set_timestamp(time(0));
  response_message.add_embed(response_embed);

  event.reply(response_message);
}
