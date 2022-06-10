#include <dpp/dpp.h>
#include <ctime>
#include <string>
#include <sstream>

const std::string BOT_TOKEN="OTgwMDc2ODg4MTI5MTU5MTk4.GrNDbL.9L3xBQ9wIUP5v9Hw5ctD6FAN4MV87sp6cXSeoo";

int main()
{
	// instantiate bot
	dpp::cluster bot(BOT_TOKEN);

	bot.on_log(dpp::utility::cout_logger());

	bot.on_slashcommand([](const dpp::slashcommand_t& event) 
	{
		if (event.command.get_command_name() == "help")
		{
			event.reply("Right now you can only '/date' me :guraW:");
		} 

        else if (event.command.get_command_name() == "date") 
        {
            time_t now = time(0);
            char* dt = ctime(&now);
            std::ostringstream oss; 
            oss << "It is now " << dt << " :xqcL";
            std::string response = oss.str();
            event.reply(response);
        }
	});

	// register for help command
	bot.on_ready([&bot](const dpp::ready_t& event) 
	{
		if (dpp::run_once<struct register_bot_commands>()) 
		{
			bot.global_command_create(dpp::slashcommand("help", "Provide usage", bot.me.id));
            bot.global_command_create(dpp::slashcommand("date", "Go on a date", bot.me.id));
        }
	});

	bot.start(false);
}

