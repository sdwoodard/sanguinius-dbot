#include <dpp/dpp.h>
#include <tcRepoCommand.hpp>

void tcRepoCommand::CommandCallBack(const dpp::message_create_t& arcEvent)
{
  dpp::message lcBotResponse;
  dpp::embed lcBotResponseEmbed = dpp::embed().
    set_color(dpp::colors::sti_blue).
    set_title("Source code for Sanguinius").
    set_url("https://github.com/sdwoodard/sanguinius-dbot").
    set_author("Github", "https://github.com/", "https://cdn-icons-png.flaticon.com/512/25/25231.png").
    set_description("Code repository for all things required to build the sanguinius discord bot.").
    set_thumbnail("https://cdn-icons-png.flaticon.com/512/25/25231.png").
    add_field("Supported by:", "Stephen Woodard").
    set_image("https://cdna.artstation.com/p/assets/images/images/020/463/234/large/adrian-prado-sanguinius-final.jpg?1567866804").
    set_footer(dpp::embed_footer().set_text("Come by sometime and try coding yourself!")).
    set_timestamp(time(0));
  lcBotResponse.add_embed(lcBotResponseEmbed);
  arcEvent.reply(lcBotResponse);

}
