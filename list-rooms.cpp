/*
SoEScriptDumper - makes SoE scripts human-readable
Copyright (C) 2020  black-sliver, neagix

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

/**

Yes, this is a strange mix of C++ and C.
It started out as a quick mostly-C-hack and is now a complete mess.
Compile with g++/cpp.

Opposed to darkmoon's output this lists all triggers for maps instead of
annotating the ROM byte by byte.
Also it tries to parse sub-instructions as part of the instructions in some
instances to add functionality or generate more readable output.

Use AUTO_DISCOVER_SCRIPTS define to get all of the scripts parsed,
or add points of interest by hand to ..scripts lists.

**/

//TODO: add readable addr/flag names to sub-instr parser
//TODO: generate useful names for auto-discovered scripts
//TODO: inline RCALLS with args (INSTR af..b4)? (re)trace af and b4
//TODO: inline (unique) 24bit CALLS?
//TODO: trace sub-instr 00 to be sure what it would do
//TODO: remove duplicate (partial) scripts from absscripts
//TODO: don't print absscripts if already inlined
//TODO: differentiate between parallel "CALLS" and actual CALLS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <list>
#include <algorithm> // std::min_element, std::max_element
#include <map>
#include <stack>
#include <assert.h>
#include <string>

#if defined(WIN32) || defined(_WIN32)
#include <process.h>
#endif

#if !defined TEXT && !defined HTML4 && !defined HTML5
#ifdef __EMSCRIPTEN__
#define HTML5 // default to HTML5 output for emscripten
#else
#define TEXT // default to (ansi colored) TEXT output on desktop
#endif
#endif

#if defined TEXT && (defined HTML4 || defined HTML5)
#error "Can only define (ansi) TEXT OR HTML output"
#endif


// define/undefine to list certain script types or other things
#define SHOW_ENTER_SCRIPTS
#define SHOW_STEP_ON_SCRIPTS
#define SHOW_B_TRIGGER_SCRIPTS
#define SHOW_ABS_ADDR
#define INCLUDE_UNKNOWN_MAPS // try all map ids
// NOTE: sniff, gourds and doggo is untested in latest version
//#define PRINT_ALL_SNIFF_SPOTS // requires SHOW_B_TRIGGER_SCRIPTS
//#define DUMP_ALL_SNIFF_SPOTS // generate sniff.h for evermizer
//#define DUMP_ALL_SNIFF_FLAGS // generate sniffflags.h for list-rooms
//#define PRINT_ALL_GOURDS // requires SHOW_B_TRIGGER_SCRIPTS
//#define DUMP_ALL_GOURDS // this is not implemented because act3 gourds suck
//#define DUMP_ALL_GOURD_FLAGS // as above
//#define DUMP_DOGGO_CHANGE // generate doggo.h for evermizer
//#define PRINT_ALL_TEXTS // try all indices for Texts
//#define TEXT_SHOW_DICT_USE
//#define PRINT_ALL_SCRIPTS // try all indices for NPC scripts
//#define NO_STATS // skip stats at the end
#define EXTENSIVE_ROOM_STATS // moar stats at the end
//#define FIND_EASTER_EGG // enable some code I used to find the easter egg
//#define PRINT_HEX // add hex dump to output
#define INLINE_RCALLS // prints out RCALLED code as part of script
#define AUTO_DISCOVER_SCRIPTS

// test compiler feature
#ifndef __has_include
#pragma message "you may have to create an empty sniffflags.inc if missing"
#endif

// output formatting
#if defined TEXT
#define START ""
#define HEADING "\033[0;93m"
#define HEADING_TEXT "\033[1;93m"
#define HEADING_END HEADING
#define GREEN "\033[92m"
#define RED "\033[91m" // this means we don't know the length. breaks out of parsing
#define UNTRACED "\033[95m" // this means we know the length, but not what it does
#define NORMAL "\033[0m"
#define END ""
#define HEX_START "\t"
#define HEX_END ""
#define LT "<"
#define GT ">"
#define LE "<="
#define GE ">="
#elif defined HTML4
#define START "<font>"// color=\"#000\">"
#define HEADING "</font><font color=\"#770\">"
#ifdef NO_BOLD
#define HEADING_TEXT
#define HEADING_END
#else
#define HEADING_TEXT "</font><font color=\"#770\"><b>"
#define HEADING_END "</b>" HEADING
#endif
#define GREEN "</font><font color=\"#0f0\">"
#define RED "</font><font color=\"#f00\">"
#define UNTRACED "</font><font color=\"#707\">"
#define NORMAL "</font><font>"// color=\"#000\">"
#define END "</font>"
#define LT "&lt;"
#define GT "&gt;"
#define LE "&lt;="
#define GE "&gt;="
#define HEX_START "<i>"
#define HEX_END "</i>"
#else // HTML5
#define START "<span class=\"n\">"
#define HEADING "</span><span class=\"h\">"
#define HEADING_TEXT "</span><span class=\"t\">"
#define HEADING_END HEADING
#define GREEN "</span><span class=\"g\">"
#define RED "</span><span class=\"r\">"
#define UNTRACED "</span><span class=\"u\">"
#define NORMAL "</span><span class=\"n\">"
#define END "</span>"
#define LT "&lt;"
#define GT "&gt;"
#define LE "&lt;="
#define GE "&gt;="
#define HEX_START "<span class=\"hex\">"
#define HEX_END "</span>"
#endif


// some utility defines
#ifndef ABS
#define ABS(a) ( ((a)<0) ? (-1*(a)) : (a) )
#endif

#ifdef SHOW_ABS_ADDR
static const bool absAddr = true;
#define ADDRFMT "0x%06x"
#else
static const bool absAddr = false;
#define ADDRFMT "+x%02x"
#endif

static bool batch = false;


#ifndef die
static void die(const char* msg)
{
    fprintf(stderr, "%s", msg);
#if (defined(WIN32) || defined(_WIN32)) && !defined(main)
    if (!batch) system("pause");
#endif
    exit(1);
}
#endif

// some constants for rom adresses
#define MAP_LIST 0x9ffde7

#ifdef AUTO_DISCOVER_SCRIPTS
static std::list<char*> strings; // buffer for generated strings
// NOTE: unique_ptr syntax is too ugly for me to handle
#endif

// NOTE: we use a list to have ordering we want
static 
#ifndef INCLUDE_UNKNOWN_MAPS
const
#endif
std::list< std::pair<uint8_t, const char*> > maps = {
    // TODO: get map names from ROM
    {0x38, "Prehistoria - South jungle / Start"},
    {0x33, "Prehistoria - Strong Heart's Exterior"},
    {0x34, "Prehistoria - Strong Heart's Hut"},
    {0x5c, "Prehistoria - Raptors"},
    {0x25, "Prehistoria - Fire Eyes' Village"},
    {0x51, "Prehistoria - Village Huts and Blimp's Hut"},
    {0x26, "Prehistoria - West area with Defend"},
    {0x5b, "Prehistoria - East jungle"},
    {0x59, "Prehistoria - Quick sand desert"},
    {0x67, "Prehistoria - Bugmuck exterior"},
    {0x16, "Prehistoria - BBM"},
    {0x17, "Prehistoria - Bug room 2"}, // all 3 levels
    {0x18, "Prehistoria - Thraxx' room"},
    {0x5a, "Prehistoria - Acid rain guy"},
    {0x41, "Prehistoria - North jungle"},
    {0x27, "Prehistoria - Mammoth Graveyard"},
    {0x69, "Prehistoria - Volcano path"},
    {0x52, "Prehistoria - Top of Volcano"}, // same for early and late game
    {0x50, "Prehistoria - Sky above Volcano"},
    {0x66, "Prehistoria - West of swamp"},
    {0x65, "Prehistoria - Swamp (main area)"},
    {0x01, "Prehistoria - Exterior of Blimp's Hut"},
    {0x3c, "Prehistoria - Volcano Room 1"},
    {0x3b, "Prehistoria - Volcano Room 2"},
    {0x3d, "Prehistoria - Pipe maze"},
    {0x3e, "Prehistoria - Side rooms of pipe maze"},
    {0x3f, "Prehistoria - Volcano Boss Room"},
    
    {0x36, "Prehistoria - Both fire pits (one room)"},
    
    {0x53, "Antiqua - Act2 Start Cutscene"},
    
    {0x0A, "Antiqua - Nobilia, Market"},
    {0x08, "Antiqua - Nobilia, Square"},
    {0x09, "Antiqua - Nobilia, Square during Aegis fight"},
    {0x1e, "Antiqua - Nobilia, Arena Holding Room"},
    {0x1d, "Antiqua - Nobilia, Arena (Vigor Fight)"},
    {0x4c, "Antiqua - Nobilia, Fountain and snake statues"},
    {0x0b, "Antiqua - Nobilia, Palace grounds"}, // TODO: better definition?
    {0x4d, "Antiqua - Nobilia, Inside palace (Horace cutscene)"},
    {0x3a, "Antiqua - Nobilia, Fire pit"},
    {0x0c, "Antiqua - Nobilia, Inn"},
    {0x1c, "Antiqua - Nobilia, North of Market"},
    
    {0x1b, "Antiqua - Desert of Doom"},
    
    {0x6b, "Antiqua - Waterfall"},
    {0x05, "Antiqua - Between 'mids and halls"}, // this reads invalid but has pots
    {0x07, "Antiqua - West of Crustacia"},
    {0x4f, "Antiqua - East of Crustacia"},
    {0x2e, "Antiqua - Blimp's Cave"},
    {0x68, "Antiqua - Crustacia exterior"},
    {0x30, "Antiqua - Crustacia inside pirate ship"},
    {0x04, "Antiqua - Crustacia fire pit"},
    {0x2f, "Antiqua - Horace's camp"},
    {0x06, "Antiqua - Outside of 'mids"},
    {0x64, "Antiqua - Cave entrance under 'mids"},
    {0x55, "Antiqua - 'mids bottom level (Dog start)"},
    {0x56, "Antiqua - 'mids top level (Boy start)"},
    {0x57, "Antiqua - 'mids basement level (Tiny)"},
    {0x58, "Antiqua - 'mids boss room (Rimsala)"},
    {0x2b, "Antiqua - Outside of halls"},
    {0x29, "Antiqua - Halls main room"},
    {0x23, "Antiqua - Halls SW"},
    {0x24, "Antiqua - Halls NW"},
    {0x2c, "Antiqua - Halls SE"},
    {0x2d, "Antiqua - Halls NE"},
    {0x28, "Antiqua - Halls Collapsing Bridge"},
    {0x2a, "Antiqua - Halls Boss Room"},
    {0x4b, "Antiqua - Oglin cave"},
    {0x6d, "Antique - Aquagoth Room"},
    
    {0x35, "Act1 Quicksand, Bugmuck and Volcano caves + Act2 West Alchemy Cave"},
    
    {0x12, "Gothica - Ebon Keep sewers"},
    {0x13, "Gothica - Between Ebon Keep sewers, Dark Forest and Swamp"},
    {0x40, "Gothica - Swamp south of Gomi's Tower"},
    {0x37, "Gothica - Gomi's Tower"},
    {0x20, "Gothica - Timberdrake room in forest"},
    {0x1f, "Gothica - Doubles room in forest"},
    {0x22, "Gothica - Dark Forest"},
    {0x6c, "Gothica - SE of Ivor Tower (Well)"},
    {0x76, "Gothica - South of Ivor Tower (Gate)"},
    {0x7b, "Gothica - Ebon Keep and Ivor Tower Exterior Bottom Half"},
    {0x7c, "Gothica - Ebon Keep and Ivor Tower Exterior Top Half"},
    {0x7d, "Gothica - Ebon Keep and Ivor Tower Interior"},
    {0x4e, "Gothica - Ivor Tower, west alley (market)"},
    {0x62, "Gothica - Ivor Tower, west square (trailers)"},
    {0x63, "Gothica - Ivor Tower, inside trailers"},
    {0x19, "Gothica - Chessboard"},
    {0x1a, "Gothica - Below chessboard"},
    {0x21, "Gothica - Dark forest entrance (save point)"},
    {0x74, "Gothica - Ebon Keep and Ivory Tower dungeon + pipe room"},
    {0x0d, "Gothica - Ebon Keep Hall (Stairs, behind Verm)"},
    {0x0f, "Gothica - Ebon Keep West Room (Naris)"},
    {0x11, "Gothica - Ebon Keep Queen's Room"},
    {0x10, "Gothica - Ebon Keep Stained Glass Hallway"},
    {0x14, "Gothica - Ebon Keep Tinker's Room"},
    {0x39, "Gothica - Ebon Keep Fire pit"},
    {0x0e, "Gothica - Ebon Keep Dining Room"},
    {0x6e, "Gothica - Ivor Tower Hall"},
    {0x6f, "Gothica - Ivor Tower Dining Room"},
    {0x70, "Gothica - Ivor Tower Exterior Bridges and Balconies"},
    {0x71, "Gothica - Ivor Tower East Room + Kitchen"},
    {0x72, "Gothica - Ivor Tower East Upper Floor"},
    {0x73, "Gothica - Ivor Tower Dog Maze Underground"},
    {0x75, "Gothica - Ivor Tower Stariwell to dungeon"},
    {0x79, "Gothica - Ivor Tower Sewers"},
    {0x7a, "Gothica - Ivor Tower Sewers Exterior (landing spot)"},
    {0x5d, "Gothica - Ebon Keep Courtyard (South of Verm)"},
    {0x5e, "Gothica - Ebon Keep Front Room (Verm)"},
    {0x5f, "Gothica - Ebon Keep Verm side rooms"},
    {0x60, "Gothica - Ebon Keep Storage Room"},
    {0x78, "Gothica - Ivor Tower Queen's Room"},
    {0x77, "Gothica - Ivor Tower Puppet Show / Mungola"},
    
    {0x46, "Omnitopia - Professor's lab and ship area, (also?) Intro"},
    {0x48, "Omnitopia - Metroplex tunnels (rimsalas, spheres)"},
    {0x44, "Omnitopia - Greenhouse (dark or both?)"},
    {0x00, "Omnitopia - Alarm room"},
    {0x43, "Omnitopia - Control room"},
    {0x45, "Omnitopia - Secret boss room"},
    {0x47, "Omnitopia - Storage room"},
    {0x42, "Omnitopia - Reactor room and Reactor control"},
    {0x54, "Omnitopia - Shops"},
    {0x7e, "Omnitopia - Jail"},
    {0x49, "Omnitopia - Junkyard (Landing spot)"},
    {0x4a, "Omnitopia - Final Boss Room"},
    
    {0x61, "Opening - Scrolling over Machine"},
    {0x31, "Intro - Podunk 1965"},
    {0x02, "Intro - Mansion Exterior 1965"},
    {0x32, "Intro - Podunk 1995"},
    {0x03, "Intro - Mansion Exterior 1995"},
};

static
#ifndef AUTO_DISCOVER_SCRIPTS
const
#endif
std::list< std::pair<uint32_t, const char*> > absscripts = {
    {0x99ebe2, "Vigor enter part [2] / script / animation"},
    {0x99ebd5, "Vigor applause?"},
    {0x99eb67, "Vigor script pt 3?"},
    {0x99ea06, "Vigor script pt 4?"},
    {0x99e9dc, "Vigor script pt 5?"},
    {0x99e417, "Vigor dead"},
    {0x95dce5, "Square outro cutscene"}, // this is RCALLed
    {0x92d92a, "Outro rain and sky color"},
    {0x96b8d9, "Timer Part ??"}, // this was found using a breakpoint
    {0x99cec1, "Timberdrake AI?"},
    {0x99cfa6, "Timberdrake AI? Part 2"},
    {0x99cf95, "Timberdrake AI? Part 3"},
    {0x99d39c, "Unknown script in Timberdrake enter"},
    {0x99bc6d, "Dark forest room change"},
    {0x99aa3d, "Dark forest room change Pt2"},
    {0x99b69b, "Dark forest room change Pt3"},
    {0x99af97, "Dark forest room change Pt4"},
    {0x99b3bf, "Dark forest room change Pt5"},
    {0x99b7df, "Dark forest room change Pt6"},
    {0x99b0bb, "Dark forest room change Pt7"},
    {0x99b36e, "Dark forest room change Pt8"},
    {0x99aa23, "Dark forest room change Pt9 (BG)"},
    {0x9581d5, "Crustacia intro"},
    {0x92a3d3, "Unknown"},
    {0x9582c9, "Crustacia intro after camera swipe"},
    {0x92a3e7, "Hide status bar layer"},
    {0x92a3ed, "Show status bar layer"},
    {0x989c84, "Queen below chessboard (Queen's Room enter script)"},
    {0x989bb5, "Queen below chessboard Pt2"},
    {0x92a3e7, "Queen below chessboard Pt3 (Also used elsewhere)"},
    {0x92de75, "Some cinematic script (used multiple times)"},
    {0x929e12, "Unknown Script 0x01"},
    {0x95b17e, "Blimp cave 1"},
    {0x95b26f, "Blimp cave 2"},
    {0x95b2ca, "Blimp cave 2a (and other2?)"},
    {0x9BC5E7, "Strange script"},
    {0x9BC5D0, "Strange script caller"},
    {0x9BC562, "Before Strange script caller (1)"},
    {0x9BC5c1, "Before Strange script caller (2)"},
    {0x9bc497, "Intro Bazooka Gourd Opened"},
    {0x9bc4dd, "Unknown #7.1"},
    {0x9bc4fa, "Unknown #7.2"},
    {0x94832c, "Top of volcano [1]"},
    {0x9483cb, "Top of volcano [2]"},
    {0x94c1c5, "FE Gathering Pt1?"},
    {0x94c213, "FE Gathering Pt2?"},
    {0x94d363, "FE Cutscene 1"},
    {0x94d606, "FE Cutscene 2"},
    {0x94da6d, "FE Cutscene 3"},
    {0x94da42, "FE Cutscene Part (Flash) [1]"},
    {0x94d849, "Huts Unknown 1"},
    {0x94dceb, "Blimp in Hut after Salabog?"},
    {0x9a845c, "Puppet show [1] (fight)"},
    {0x9a8aea, "Puppet show [2] (no fight)"},
    {0x93c702, "Post thraxx cutscene"},
    {0x93ca9f, "Thraxx maggot trigger part"},
    {0x93ca75, "Thraxx enter part [1]"},
    {0x93ca69, "Thraxx enter part [2]"},
    {0x93c7f7, "Thraxx enter part [3]"},
    {0x93cb12, "Thraxx enter part [4]"},
    {0x93cb65, "Thraxx enter part [5]"},
    {0x94a35a, "Hardball dialog"},
    {0x9bdac9, "Prof. Lab part [1]"},
    {0x9bd074, "Prof. Lab part [2]"},
    {0x9bbcd9, "Prof. Lab part [3]"},
    {0x9bcdda, "Prof. Lab Call bead + Call up cutscene"},
    {0x9bc8d5, "Prof. Lab part [5]"},
    {0x9bc83a, "Prof. Lab part [6]"},
    {0x93ae82, "Acid rain dialog"},
    {0x95d248, "Palace cutscene part [1]"},
    {0x95d065, "Palace cutscene part [2]"},
    {0x95d31b, "Palace cutscene part [3]"},
    {0x95ba3f, "Crush dialog"},
    {0x96808f, "Market (timer expired?) part [1]"},
    {0x96811a, "Market (timer expired?) part [2]"},
    {0x9680ab, "Market (timer expired?) part [3]"},
    {0x93dacd, "Mammoth graveyard dialog"},
    {0x98d26c, "Lance cutscene"},
    {0x948000, "Top of volcano dialog"},
    {0x99a604, "Gomi's tower part [1]"},
    {0x99a377, "Gomi's tower part [2]"},
    {0x9996f0, "Lightning storm?"},
    {0x97819a, "Desert of Doom part [1]"},
    {0x96e3fe, "Desert of Doom part [2]"},
    {0x9bde41, "Junkyard landing?"},
    {0x92de7e, "Omnitopia hatch fade-in?"},
    {0x9ae24f, "Omnitopia hatch fade-out?"},
    {0x92bebd, "Loot money"},
    {0x97e060, "Aquagoth part? [1]"},
    {0x97e039, "Aquagoth part? [2]"},
    {0x98a862, "Prison door 7 with $2834&0x01 set"},
    {0x98a52c, "Prison door 7 with $2834&0x01 set Pt2"},
    {0x98a5ed, "Prison door 1 west castle [1]"},
    {0x98a607, "Prison door 1 west castle [2]"},
    {0x98ad85, "Entering prison part (Getting thrown in cutscene)"},
    {0x93d036, "Thraxx damage / kill part [1]"},
    {0x93ce25, "Thraxx damage / kill part [2]"},
    {0x92d7b1, "Fade to/from/flash white A?"},
    {0x92d723, "Fade to/from/flash white B?"},
    {0x92d752, "Fade to/from/flash white C?"},
    {0x92bf33, "Hold up weapon"},
    {0x94a6f5, "Cave raptors kill part [1] ??"},
    {0x92d594, "Megataur/Rimsala/DE reward"},
    {0x9589d3, "'mids [4] (sons?) part"},
    {0x99c3ff, "Doubles room enter part [1]"},
    {0x99c53e, "Doubles room enter part [2]"},
    {0x92d519, "Timberdrake Kill part [1]"},
    {0x92df70, "Boss kill part"},
    {0x95a8a4, "Tiny teleport script?"},
    {0x95a7c2, "Tiny teleport script? part [1]"},
    {0x95a899, "Tiny teleport script? part [2]"},
    {0x95a740, "Tiny teleport script? part [3]"},
    {0x95a75c, "Tiny teleport script? part [4]"},
    {0x959e45, "Tiny DE cutscene part? [1]"},
    {0x959efc, "Tiny DE cutscene part? [2]"},
    {0x95a329, "Tiny DE cutscene reward"},
    {0x959f00, "Tiny DE cutscene part? [4]"},
    {0x959e4d, "Tiny DE cutscene part? [5]"},
    {0x94bb17, "Magmar intro part [1]"},
    {0x94bc20, "Magmar intro part [2]"},
    {0x94ba3b, "Magmar intro part [3]"},
    {0x94baa9, "Magmar intro part [4]"},
    {0x94bd53, "Magmar outro"},
    {0x998c35, "Tinker part [1]"},
    {0x998942, "Tinker part [2]"},
    {0x9988de, "Tinker part [2b]"},
    {0x998910, "Tinker part [2c]"},
    {0x9986c4, "Tinker part [3]"},
    {0x99835b, "Tinker part [4]"},
    {0x9989a4, "Tinker part [5] (Rocket done?)"},
    {0x9a926d, "Mungola drop (found w/ hex editor)"}, // 0x9a9280 = text message
    {0x9be385, "Beyond end"},
    {0x9be38a, "End of Beyond end"},
    {0x96d6cf, "Horace pit cutscene"},
    {0x92cfe7, "Showcase Thraxx"},
    {0x92a3d3, "Intro part?"},
    {0x92a3e5, "Intro part?"},
    {0x92e401, "Intro part? running in BG"},
    {0x998956, "Giving Tinker rocket parts"},
    {0x998b39, "Dialog after giving Tinker rocket parts"},
#ifdef RANDO
    {0xb08018, "Select @Corrosion"},
    {0xb08030, "Select @Double Drain"},
    {0xb08036, "Select @Drain"},
#endif
    {0xb0800c, "Select @Barrier"},
    {0x94cd21, "FE Village call outro"},
    {0x94ca42, "FE Village actual outro"},
    {0x94cd25, "FE Village credits"},
    {0x95d9dd, "Sacred dog cutscene"},
    {0x92cc6c, "Unknown script in square"},
#ifdef RANDO
    {0x97c144, "Randomizer Vigor fix"},
    {0x95d9e5, "Randomizer Sacred dog cutscene"}, // part of Sacred dog cutscene in vanilla
#endif
    {0xb08070, "Rando Jukebox 1"},
    {0xb08079, "Rando Jukebox 2"},
    {0xb08084, "Rando Jukebox 3"},
    {0xb08091, "Rando Jukebox impl"},
    {0x95b945, "East of Crustacia Rock landing"},
    {0x95baaa, "East of Crustacia Enter Part 2"},
    {0x94de7d, "Blimp (in Hut) Part"},
    {0x93887d, "Progress Raptors"},
    {0x9abed9, "Banquet cutscene 1"},
    {0x9ac0f7, "Banquet cutscene 2"},
    {0x97edd8, "Queen's Key Door"},
    {0x95c53c, "North of Market guard"},
    {0xb18951, "Added gourdomizer drops"},
    {0x98daa0, "Cecil first talk"},
    {0x98db47, "Cecil first talk part"},
    {0x98dc2c, "Cecil shop"},
    {0x98dc9d, "Get Cecil's Bazooka"},
    {0x98be3a, "Don't buy"},
    {0x92cdb3, "Can't buy (not enough ...)"},
    {0x92cdbe, "Can't buy (inventory full)"},
    {0x99a861, "Dark forest sniff #0"}, 
    {0x99a8bb, "Dark forest sniff #1"},
    {0x99a9e7, "Dark forest sniff #2"},
    {0x99a951, "Dark forest sniff #3"},
    {0x99a8f7, "Dark forest sniff #4"},
    {0x99a96f, "Dark forest sniff #5"},
    {0x99aa05, "Dark forest sniff #6"},
    {0x99a87f, "Dark forest sniff #7"},
    {0x99a8d9, "Dark forest sniff #8"},
    {0x99a9c9, "Dark forest sniff #9"},
    {0x99a915, "Dark forest sniff #10"},
    {0x99a9ab, "Dark forest sniff #11"},
    {0x99a98d, "Dark forest sniff #12"},
    {0x99a89d, "Dark forest sniff #13"},
    {0x99a933, "Dark forest sniff #14"},
    {0x92db2a, "Unknown world map? #1"},
    {0x92db35, "Unknown world map? #2"},
    {0x92db56, "Unknown world map? #3"},
    {0x92db5c, "Unknown world map? #4"},
    {0x92db61, "Unknown world map? #5"},
    {0x92db66, "Unknown world map? #6"},
    {0x92db6b, "Unknown world map? #7"},
    {0x92db70, "Unknown world map? #8"},
    {0x92db75, "WW landing (found w/ breakpoint)"},
    {0x968416, "Fish guy cure formula"},
    {0x959de0, "Open Tiny's gate (3 switches)"},
    {0x99efce, "Verm spell cast"},
    {0x9b814e, "Generate act4 numeric codes"},
    {0x95c0c4, "Shop 10,000 jewel Amulet from Monk"},
    {0x95c0b3, "Unload 10,000 jewel Amulet Monk"},
    {0x939b29, "open 1st/cave/11th/last-center sand whirl"},
    {0x939dde, "close 1st/cave/11th/last-center sand whirl"},
    {0x939b68, "open 2nd/12th/last-right sand whirl"},
    {0x939e09, "close 2nd/12th/last-right sand whirl"},
    {0x939d21, "open 3rd/13th sand whirl"},
    {0x939f36, "close 3rd/13th sand whirl"},
    {0x939be6, "open 4th/14th sand whirl"},
    {0x939e5f, "close 4th/14th sand whirl"},
    {0x939ba7, "open far-right/last-left sand whirl"},
    {0x939e34, "close far-right/last-left sand whirl"},
    {0x939d60, "open below-far-right sand whirl"},
    {0x939f61, "close below-far-right sand whirl"},
    {0x939c25, "open 5th/15th sand whirl"},
    {0x939e8a, "close 5th/15th sand whirl"},
    {0x939d9f, "open nw-of-far-right sand whirl"},
    {0x939f8c, "close nw-of-far-right sand whirl"},
    {0x939ce2, "open left-of-far-right sand whirl"},
    {0x939f0b, "close left-of-far-right sand whirl"},
    {0x939c64, "open 6th sand whirl"},
    {0x939eb5, "close 6th sand whirl"},
    {0x93a59a, "Sand whirl script part [1]"},
    {0x93a524, "Sand whirl script part [2]"},
    {0x9997a7, "Sterling battle over, Gomi introduction"},
    {0x96e286, "Desert of Doom shuttle - random name-calling"},
    {0x978000, "Desert of Doom shuttle - Crustacia destination"},
    {0x96ef0d, "Desert of Doom shuttle - Nobilia destination"},
    {0x95e8a1, "Nobilia market - hidden medallion sale"},
    {0x969b8a, "Nobilia market - appraisal dialog"},
    {0x92cd9e, "Nobilia market - no trade with dog"},
    {0x92cdac, "Nobilia market - no trade with dog"},
    {0x92cd89, "Nobilia market - no trade with dog"},
    {0x92cd90, "Nobilia market - no trade with dog"},
    {0x92cd97, "Nobilia market - no trade with dog"},
    {0x96ad2a, "Nobilia market - prophet easter egg dialog"},
};
static
#ifndef AUTO_DISCOVER_SCRIPTS
const
#endif
std::list< std::pair<uint16_t, const char*> > npcscripts = {
    {0x17cd, "Thraxx damage/kill"},
    {0x199e, "Aegis kill"},
    {0x1a79, "Vigor damage"},
    {0x1a70, "Footknight kill"},
    {0x1a82, "Puppet damage/kill"},
    {0x1a85, "Mungola? damage/kill"},
    {0x19b0, "Aquagoth"},
    {0x180c, "Jaguar ring dude"},
    {0x1809, "HB Guy"},
    {0x180f, "Cave NPC 3"},
    {0x1812, "Cave NPC 4"},
    {0x1818, "Cave raptors kill"},
    {0x1815, "Drain Guy"},
    {0x1b72, "Prof. Ruffelburg"},
    {0x17b8, "Acid Rain Guy"},
    {0x19cb, "Corrosion Guy"},
    {0x1860, "Defend Guy"},
    {0x1968, "Horace in Camp?"},
    {0x196b, "Horace Camp Madronius"},
    {0x196e, "Act2/Horace camp Inn keeper(s)"},
    {0x1992, "Madronius' Brother in Ruins"},
    {0x19b3, "Fire Power Dude"},
    {0x1a73, "Dude below chessboard"},
    {0x17d3, "Mammoth Viper 1"},
    {0x17d6, "Mammoth Viper 2"},
    {0x17d9, "Mammoth Viper 3"},
    {0x17dc, "Mammoth Viper 4"},
    {0x17df, "Mammoth Viper Commander"},
    {0x1a52, "Sterling"},
    {0x1ad3, "Naris"},
    {0x1971, "Sting Man"},
    {0x1806, "Volcano Room1 NPC 1"},
    {0x1803, "Speed dude"},
    {0x1863, "Strong Heart (inside Hut)"},
    {0x18bd, "Blimp (in cave)"},
    {0x185d, "Blimp (in hut)"},
    {0x1a46, "Regrowth Lady"},
    {0x1a58, "One Up Guy"},
    {0x1b75, "Junkyard Robot / Reflect"},
    {0x17af, "Raptors kill"},
    {0x19c5, "Doggo dies in prison?"},
    {0x1980, "Salabog damage/kill"},
    {0x17f4, "Viper commander kill"},
    {0x1794, "Halls Wings"},
    {0x1797, "Tiny Wings"},
    {0x198f, "Unknown script in Halls NE"},
    {0x1995, "Halls Mad Monk Kill"},
    {0x1998, "Megataur kill"},
    {0x18a5, "Rimsala kill"},
    {0x187b, "'mids [1]"},
    {0x187e, "'mids [2]"},
    {0x172b, "Unknown 0eac+0 (set in lots of places)"},
    {0x1872, "'mids [4] (sons?)"},
    {0x1a6a, "Doubles room save point"},
    {0x1a5e, "Doubles kill (first?)"},
    {0x1a61, "Doubles kill (others?)"},
    {0x1a64, "Doubles kill (last?)"},
    {0x1a6d, "Timberdrake Kill"},
    {0x1a7c, "Verm kill"},
    {0x181e, "Magmar damage"},
    {0x1a4f, "Tinker"},
    {0x172b, "Unknown eac+0 script. Prize drops?"},
    {0x178e, "BBM Wings"},
    {0x1857, "Fire Eyes"},
    {0x1845, "Hut NPC 1"},
    {0x1848, "Hut NPC 2"},
    {0x184b, "Hut NPC 3"},
    {0x184e, "Hut NPC 4"},
    {0x1851, "Hut NPC 5"},
    {0x1854, "Hut NPC 6"},
    {0x185a, "Hut NPC 7"},
    {0x1842, "FE Village NPC1"},
    {0x1827, "FE Village NPC2/Bee Boy"},
    {0x1839, "FE Village NPC3"},
    {0x182a, "FE Village NPC4"},
    {0x1824, "FE Village NPC5"},
    {0x1836, "FE Village NPC6"},
    {0x1833, "FE Village NPC7"},
    {0x182d, "FE Village NPC8"},
    {0x183f, "FE Village NPC9"},
    {0x183c, "FE Village NPC10"},
    {0x1830, "FE Village NPC11"},
    {0x1821, "FE Village NPC12"},
#if defined INCLUDE_MARKET || defined AUTO_DISCOVER_SCRIPTS
    {0x191d, "Market NPC 1"},
    {0x1920, "Market NPC 2"},
    {0x1944, "Market NPC 3"},
    {0x1923, "Market NPC 4"},
    {0x1929, "Market NPC 5"},
    {0x192c, "Market NPC 6"},
    {0x194d, "Market NPC 7"},
    {0x192f, "Market NPC 8"},
    {0x1932, "Market NPC 9"},
    {0x1947, "Market NPC 10"},
    {0x1935, "Market NPC 11"},
    {0x1950, "Market NPC 12"},
    {0x1938, "Market NPC 13"},
    {0x1953, "Market NPC 14"},
    {0x1941, "Market NPC 15"},
    {0x193e, "Market NPC 16"},
    {0x193b, "Market NPC 17"},
    {0x1926, "Market NPC 18"},
#endif
    {0x18f3, "North of Market Tiny dialog"},
    {0x19b6, "Dog Maze Lady"},
    {0x1a43, "Act3 shop?"},
    {0x1a07, "Lance dialog"},
    {0x1a22, "Act3 Houses NPC1"},
    {0x1a28, "Act3 Houses NPC2"},
    {0x1a0a, "Act3 Houses NPC3"},
    {0x1a1c, "Act3 Houses NPC4"},
    {0x1a0d, "Act3 Houses NPC5"},
    {0x1a13, "Act3 Houses NPC6"},
    {0x1a2b, "Act3 Houses NPC7"},
    {0x1a37, "Act3 Houses NPC8"},
    {0x1a10, "Act3 Houses NPC9"},
    {0x1a31, "Act3 Houses NPC10"},
    {0x1a16, "Act3 Houses NPC11"},
    {0x1a07, "Act3 Houses NPC12"},
    {0x1a3a, "Act3 Houses NPC13"},
    {0x1a19, "Act3 Houses NPC14"},
    {0x1a3d, "Act3 Houses NPC15"},
    {0x1a1f, "Act3 Houses NPC16"},
    {0x1a40, "Act3 Houses NPC17"},
    {0x1a34, "Act3 Houses NPC18"},
    {0x1a2e, "Act3 Houses NPC19"},
    {0x1a25, "Act3 Houses NPC20"},
    {0x1ad0, "Ivor Tower Well Guy"},
};
static
#ifndef AUTO_DISCOVER_SCRIPTS
const
#endif
std::list< std::pair<uint8_t, const char*> > globalscripts = {
    {0x00, "Fade-out / stop music"},
    {0x01, "Fade-in / start music"},
    {0x02, "Open message box?"},
    {0x07, "Open message box?"},
    {0x0a, "Open message box?"},
    {0x0b, "Open message box?"},
    {0x0d, "Prepare unframed text during Aquagoth defeat"},
    {0x19, "Prepare room change? West exit/east entrance outdoor-outdoor?"}, // outdoor<->indoor is probably actually change music or not
    {0x20, "Prepare room change? East exit/north entrance"},
    {0x1d, "Prepare room change? East exit/west entrance outdoor-outdoor?"},
    {0x21, "Prepare room change? South exit/north entrance outdoor-outdoor?"},
    {0x22, "Prepare room change? South exit/north entrance indoor-outdoor?"},
    {0x26, "Prepare room change? North exit/south entrance outdoor-indoor?"},
    {0x27, "Prepare room change? North exit/south entrance indoor-outdoor?"},
    {0x27, "Prepare room change? Falling?"},
    {0x2a, "Prepare room change? North exit/north entrance"},
    {0x32, "Market NPC talk (and others?)"},
    {0x33, "Market NPC end (and others?)"},
    {0x39, "Loot nature?"},
    {0x3a, "Loot gourd?"},
    {0x3b, "Loot other gourd ?"},
    {0x3e, "Loot part1?"},
    {0x3f, "Loot part2?"},
    {0x41, "Loot part3?"},
    {0x48, "Open shop menu?"},
    {0x4b, "Open shop menu Pt2?"},
    {0x4c, "Ask to equip spells"},
    {0x4d, "Save dialog"},
    {0x4e, "Actual save dialog"},
    {0x4f, "Actual saving"},
    {0x54, "Buy ingredients dialog"},
    {0x59, "Attraction mode, after Thraxx"},
    {0x5a, "Credits"},
    {0xff, "Salabog dead??"},
};

static const std::map<uint16_t, const char*> ram = {
    { 0x2391, "PRIZE    ($2391)" },
    { 0x2393, "AMOUNT   ($2393)" },
    { 0x2395, "MAP REF? ($2395)" }, // this is some value used in sub-scripts, possibly to disable/change the entity/map object/sprite
    { 0x2461, "NEXT ADD ($2461)" },
    { 0x238d, "CHANGE MUSIC ($238d)" },
    { 0x23c3, "DOGGO CLOSE  ($23c3)" },
    { 0x2443, "CHANGE DOGGO ($2443)" },
    { 0x234f, "PRISON TOWN EXIT ($234f)" },
    { 0x22fd, "DESERT WRAP X? ($22fd)" },
    { 0x22fc, "DESERT WRAP Y? ($22fc)" },
    { 0x234b, "Hut/house to enter ($234b)" },
    { 0x2441, "GET WEAPON ($2441)" },
    { 0x00c3, "SCREEN SHAKING ($00c3)" },
    { 0x2409, "SCREEN SHAKING MAGNITUDE X ($2409)" },
    { 0x240b, "SCREEN SHAKING MAGNITUDE Y ($240b)" },
    { 0x2445, "PRESELECT ALCHEMY ($2445)" },
    { 0x239b, "PRIZE 1 RATE ($239b)" },
    { 0x239d, "PRIZE 2 RATE ($239d)" },
    { 0x239f, "PRIZE 3 RATE ($239f)" },
    { 0x23a1, "PRIZE 1 DROP ($23a1)" },
    { 0x23a3, "PRIZE 2 DROP ($23a3)" },
    { 0x23a5, "PRIZE 3 DROP ($23a5)" },
    { 0x23a7, "PRIZE 1 QTY  ($23a7)" },
    { 0x23a9, "PRIZE 2 QTY  ($23a9)" },
    { 0x23ab, "PRIZE 3 QTY  ($23ab)" },
    { 0x2449, "SAVE SPOT ($2449)" },
    { 0x2459, "INGREDIENT SHOP ($2459)" },
    { 0x23e9, "MAP X start ($23e9)" },
    { 0x23eb, "MAP Y start ($23eb)" },
    { 0x23ed, "MAP X end   ($23ed)" },
    { 0x23ef, "MAP Y end   ($23ef)" },
    { 0x2352, "Queen's Key on Dog ($2352)" },
    { 0x2353, "Queen's Key on Boy ($2353)" },
};

static const std::map<uint16_t, const char*> prizes = {
    {0x0000, "Nothing (0x0000)"},
    {0x0001, "Money (0x0001)"},
    {0x0101, "Chocobo Egg? (0x0101)"}, // OTHER CHARMS ? This may read another address
    // rando exclusives:
    {0x0103, "Rando Thug's Cloak (0x0103)"},
    {0x0117, "Rando Knight Basher (0x0117)"},
    {0x0118, "Rando Atom Smasher (0x0118)"},
    {0x011c, "Rando Laser Lance (0x011c)"},
    {0x0125, "Rando Gauge (0x0125)"},
    {0x0131, "Rando Amulet of A. (0x0131)"},
    {0x01f1, "Rando Colosseum Breast (0x01f1)"},
    {0x01f2, "Rando Colosseum Gloves (0x01f2)"},
    {0x01f3, "Rando Colosseum Helmet (0x01f3)"},
    {0x01f4, "Rando progressive Breast (0x01f4)"},
    {0x01f5, "Rando progressive Gloves (0x01f5)"},
    {0x01f6, "Rando progressive Helmet (0x01f6)"},
    // 0x0200..0x020f: ingredients
    {0x0200, "Wax (0x0200)"},
    {0x0201, "Water (0x0201)"},
    {0x0202, "Vinegar (0x0202)"},
    {0x0203, "Roots (0x0203)"},
    {0x0204, "Oil (0x0204)"},
    {0x0205, "Mud Pepper (0x0205)"},
    {0x0206, "Mushroom (0x0206)"},
    {0x0207, "Meteorite (0x0207)"},
    {0x0208, "Limestone (0x0208)"},
    {0x0209, "Iron (0x0209)"},
    {0x020a, "Gunpowder (0x020a)"},
    {0x020b, "Grease (0x020b)"},
    {0x020c, "Feather (0x020c)"},
    {0x020d, "Ethanol (0x020d)"},
    {0x020e, "Dry Ice (0x020e)"},
    {0x020f, "Crystal (0x020f)"},
    {0x0210, "Clay (0x0210)"},
    {0x0211, "Brimstone (0x0211)"},
    {0x0212, "Bone (0x0212)"},
    {0x0213, "Atlas Medallion (0x0213)"},
    {0x0214, "Ash (0x0214)"},
    {0x0215, "Acorns (0x0215)"}, // that's all.
    // 0x0401..0x0428: equippment
    // what is 0x0400 ??
    {0x0401, "Grass Vest (0x0401)"},
    {0x0402,  "Shell Plate (0x0402)"},
    {0x0403,  "Dino Skin (0x0403)"},
    {0x0404,  "Bronze Chestplate (0x0404)"},
    {0x0405,  "Stone Vest (0x0405)"},
    {0x0406,  "Centurian Cape (0x0406)"},
    {0x0407,  "Silver Mail (0x0407)"},
    {0x0408, "Gold-Plated Vest (0x0408)"},
    {0x0409,  "Shining Armor (0x0409)"},
    {0x040a,  "Magna Mail (0x040a)"},
    {0x040b, "Titanium Vest (0x040b)"},
    {0x040c,  "Virtual Vest (0x040c)"},
    {0x040d,  "Grass Hat (0x40d)"},
    {0x040e, "Shell Hat (0x040e)"},
    {0x040f,  "Dinosaur Helmet (0x040f)"},
    {0x0410,  "Bronze Helmet (0x0410)"},
    {0x0411,  "Obsidian Helmet (0x0411)"},
    {0x0412,  "Centurian Helmet (0x0412)"},
    {0x0413,  "Titan's Crown (0x0413)"},
    {0x0414,  "Dragon Helmet (0x0414)"},
    {0x0415,  "Knight's Helmet (0x0415)"},
    {0x0416,  "Lightning Helmet (0x0416)"},
    {0x0417, "Old Reliable (0x0417)"},
    {0x0418,  "Brainstorm Helmet (0x0418)"},
    {0x0419,  "vine bracelet (0x0419)"},
    {0x041a, "Mammoth Guard (0x041a)"},
    {0x041b,  "Claw Guard (0x041b)"},
    {0x041c,  "Serpent Bracelet (0x041c)"},
    {0x041d,  "Bronze Gauntlet (0x041d)"},
    {0x041e,  "Gloves of Ra (0x041e)"},
    {0x041f, "Iron Bracer (0x041f)"},
    {0x0420,  "Magician's Ring (0x0420)"},
    {0x0421,  "Dragon's Claw (0x0421)"},
    {0x0422,  "Cyber Glove (0x0422)"},
    {0x0423, "Protector Ring (0x0423)"},
    {0x0424,  "Virtual Glove (0x0424)"},
    {0x0425,  "Leather Collar (0x0425)"},
    {0x0426,  "Spiky Collar (0x0426)"},
    {0x0427,  "Crusader Collar (0x0427)"},
    {0x0428,  "Spot's COllar (0x0428)"},
    // 0x0800..0x0807: items
    {0x0800, "Petal (0x0800)"},
    {0x0801, "Nectar (0x0801)"},
    {0x0802, "Honey (0x0802)"},
    {0x0803, "Biscuit (0x0803)"},
    {0x0804, "Wings (0x0804)"},
    {0x0805, "Herbal Essence (0x0805)"},
    {0x0806, "Pixie Dust (0x0806)"},
    {0x0807, "Call Beads (0x0807)"}, // that's all.
    // WEAPONS ?
    // BAZOOKA AMMUNITION ?
    // SPELLS LEARNED ?
};
static const std::map<uint16_t, const char*> doggos = {
    {0x02, "Wolf (0x02)"},
    {0x04, "Also Wolf (0x04)"},
    {0x06, "Greyhound (0x06)"},
    {0x08, "Poodle (0x08)"},
    {0x0A, "Regular (0x0A)"},
    {0x0C, "Toaster (0x0C)"},
    {0x0E, "Softlock (0x0E)"},
};
static const std::map<uint16_t, const char*> weapons = {
    {0x02, "Bone Crusher (0x02)"},
    {0x04, "Gladiator Sword (0x04)"},
    {0x06, "Crusader Sword (0x06)"},
    {0x08, "Neutron Blade (0x08)"},
    {0x0a, "Spider Claw (0x0a)"},
    {0x0c, "Bronze Axe (0x0c)"},
    {0x0e, "Knight Basher (0x0e)"},
    {0x10, "Atom Smasher (0x10)"},
    {0x12, "Horn Spear (0x12)"},
    {0x14, "Bronze Spear (0x14)"},
    {0x16, "Lance (0x16)"},
    {0x18, "Laser Lance (0x18)"},
    {0x1a, "Bazooka (0x1a)"},
    {0x1c, "Eqip bazooka without having it in inventory (different shell?) (0x1c)"},
    {0x1e, "Eqip bazooka without having it in inventory (different shell?) (0x1e)"},
    //{0x20, "Hard lock"},
    {0xffff, "REMOVE all weapons (0xFFFF)"}
};
static const std::map<uint16_t, const char*> alchemy_preselections = {
    {0x00, "Acid Rain (0x00)"},     {0x02, "Atlas (0x02)"},
    {0x04, "Barrier (0x04)"},       {0x06, "Call Up (0x06)"},
    {0x08, "Corrosion (0x08)"},     {0x0a, "Crush (0x0a)"},
    {0x0c, "Cure (0x0c)"},          {0x0e, "Defend (0x0e)"},
    {0x10, "Double Drain (0x10)"},  {0x12, "Drain (0x12)"},
    {0x14, "Energize (0x14)"},      {0x16, "Escape (0x16)"},
    {0x18, "Explosion (0x18)"},     {0x1a, "Fireball (0x1a)"},
    {0x1c, "Fire Power (0x1c)"},    {0x1e, "Flash (0x1e)"},
    {0x20, "Force Field (0x20)"},   {0x22, "Hard Ball (0x22)"},
    {0x24, "Heal (0x24)"},          {0x26, "Lance (0x26)"},
    {0x2a, "Levitate (0x2a)"},      {0x2c, "Lightning Storm (0x2c)"},
    {0x2e, "Miracle Cure (0x2e)"},  {0x30, "Nitro (0x30)"},
    {0x32, "One Up (0x32)"},        {0x34, "Reflect (0x34)"},
    {0x36, "Regrowth (0x36)"},      {0x38, "Revealer (0x38)"},
    {0x3a, "Revive (0x3a)"},        {0x3c, "Slow Burn (0x3c)"},
    {0x3e, "Speed (0x3e)"},         {0x40, "Sting (0x40)"},
    {0x42, "Stop (0x42)"},          {0x44, "Super Heal (0x44)"},
};
static const std::map<uint16_t, const char*> save_spots = {
    {0x0f, "Madronius' brother (0x0f)"},
};
static const std::map<uint16_t, std::map<uint16_t, const char*> > ramvalues = {
    {0x2391, prizes},
    {0x2441, weapons},
    {0x2443, doggos},
    {0x234b, { {0x09, "Blimp's hut (0x09)"} }},
    {0x2445, alchemy_preselections},
    {0x2449, save_spots},
};

static
#if __cplusplus>=201907L // or 17?
constexpr
#endif
uint8_t bm2bp(const uint8_t bp) {
    /*static_*/assert(bp==0x01||bp==0x02||bp==0x04||bp==0x08||
                      bp==0x10||bp==0x20||bp==0x40||bp==0x80);
    switch (bp) {
        case 0x01:
            return 0;
        case 0x02:
            return 1;
        case 0x04:
            return 2;
        case 0x08:
            return 3;
        case 0x10:
            return 4;
        case 0x20:
            return 5;
        case 0x40:
            return 6;
        case 0x80:
            return 7;
        default:
            return 0xff; // error
    }
}
        
static const std::map<std::pair<uint16_t, uint8_t>, const char*> flags = {
    // bp2bm: 0:0x01, 1:0x02, 2:0x04, 3:0x08, 4:0x10, 5:0x20, 6:0x40, 7:0x80
    {{0x2258, bm2bp(0x01)}, "Acid Rain"},
    {{0x2258, bm2bp(0x02)}, "Atlas"},
    {{0x2258, bm2bp(0x04)}, "Barrier"},
    {{0x2258, bm2bp(0x08)}, "Call Up"},
    {{0x2258, bm2bp(0x10)}, "Corrosion"},
    {{0x2258, bm2bp(0x20)}, "Crush"},
    {{0x2258, bm2bp(0x40)}, "Cure"},
    {{0x2258, bm2bp(0x80)}, "Defend"},
    {{0x2259, bm2bp(0x01)}, "Double Drain"},
    {{0x2259, bm2bp(0x02)}, "Drain"},
    {{0x2259, bm2bp(0x04)}, "Energize"},
    {{0x2259, bm2bp(0x08)}, "Escape"},
    {{0x2259, bm2bp(0x10)}, "Explosion"},
    {{0x2259, bm2bp(0x20)}, "Fireball"},
    {{0x2259, bm2bp(0x40)}, "Fire Power"},
    {{0x2259, bm2bp(0x80)}, "Flash"},
    {{0x225a, bm2bp(0x01)}, "Force Field"},
    {{0x225a, bm2bp(0x02)}, "Hard Ball"},
    {{0x225a, bm2bp(0x04)}, "Heal"},
    {{0x225a, bm2bp(0x08)}, "Lance"},
    {{0x225a, bm2bp(0x20)}, "Levitate"},
    {{0x225a, bm2bp(0x40)}, "Lightning Storm"},
    {{0x225a, bm2bp(0x80)}, "Miracle Cure"},
    {{0x225b, bm2bp(0x01)}, "Nitro"},
    {{0x225b, bm2bp(0x02)}, "One Up"},
    {{0x225b, bm2bp(0x04)}, "Reflect"},
    {{0x225b, bm2bp(0x08)}, "Regrowth"},
    {{0x225b, bm2bp(0x10)}, "Revealer"},
    {{0x225b, bm2bp(0x20)}, "Revive"},
    {{0x225b, bm2bp(0x40)}, "Slow Burn"},
    {{0x225b, bm2bp(0x80)}, "Speed"},
    {{0x225c, bm2bp(0x01)}, "Sting"},
    {{0x225c, bm2bp(0x02)}, "Stop"},
    {{0x225c, bm2bp(0x04)}, "Super Heal"},
    {{0x225c, bm2bp(0x08)}, "Rando: Aquagoth done"}, // unused in vanilla
    {{0x225c, bm2bp(0x10)}, "Rando: Volcano viper done"}, // unused in vanilla?
    {{0x225c, bm2bp(0x20)}, "Rando: Bronze spear monk done"}, // unused in vanilla?
    {{0x225c, bm2bp(0x40)}, "FE call beads?"},
    {{0x225c, bm2bp(0x80)}, "Horace call beads"},
    {{0x225d, bm2bp(0x01)}, "Camellia call beads"},
    {{0x225d, bm2bp(0x02)}, "Prof. callbeads"},
    {{0x225d, bm2bp(0x04)}, "Desert spin???"},
    {{0x225d, bm2bp(0x08)}, "Market timer expired"},
    {{0x225d, bm2bp(0x10)}, "Unknown Market flag/loot?"},
    {{0x225d, bm2bp(0x20)}, "Unknown Market flag"},
    {{0x225d, bm2bp(0x40)}, "Unknown Market flag"},
    {{0x225d, bm2bp(0x80)}, "Some Volcano Room2 flag?"},    
    {{0x225e, bm2bp(0x01)}, "Volcano Room2 Flag"},
    {{0x225e, bm2bp(0x02)}, "Volcano Room2 Flag"},
    {{0x225e, bm2bp(0x04)}, "Volcano Room2 Flag"},
    {{0x225e, bm2bp(0x08)}, "Volcano Room2 Flag"},
    {{0x225e, bm2bp(0x10)}, "Volcano Room2/ Flag"},
    {{0x225e, bm2bp(0x20)}, "Checked in North Jungle"},
    {{0x225e, bm2bp(0x40)}, "Volcano - Viper commander spawn?"},
    {{0x225e, bm2bp(0x80)}, "Talked to Blimp in hut?"},
    {{0x225f, bm2bp(0x01)}, "Doggo palace cutscene watched"},
    {{0x225f, bm2bp(0x02)}, "???"}, // unused?
    {{0x225f, bm2bp(0x04)}, "Some Mammoth Graveyard fight flag?"},
    {{0x225f, bm2bp(0x08)}, "Pipe Maze switch pressed?"},
    {{0x225f, bm2bp(0x10)}, "Pipe Maze Raptor shown?"},
    {{0x225f, bm2bp(0x20)}, "Vigor defeated"},
    {{0x225f, bm2bp(0x40)}, "Act1 Raptors fought"},
    {{0x225f, bm2bp(0x80)}, "Village post-thraxx message shown?"},
    {{0x2260, bm2bp(0x01)}, "???"}, // unused?
    {{0x2260, bm2bp(0x02)}, "???"}, // unused?
    {{0x2260, bm2bp(0x04)}, "Unknown, changes speed dude's dialog"},
    {{0x2260, bm2bp(0x08)}, "Thraxx maggots triggered"},
    {{0x2260, bm2bp(0x10)}, "Thraxx dead"},
    {{0x2260, bm2bp(0x20)}, "Magmar fight started?"},
    {{0x2260, bm2bp(0x40)}, "Magmar dead"},
    {{0x2260, bm2bp(0x80)}, "Unknown Crustacia indoors flag"},
    
    {{0x2261, bm2bp(0x01)}, "Dog unavailable"},
    {{0x2261, bm2bp(0x02)}, "Boy unavailable"},
    
    {{0x2261, bm2bp(0x04)}, "Unknown Market flag"},
    {{0x2261, bm2bp(0x08)}, "Unknown Market flag"},
    {{0x2261, bm2bp(0x10)}, "Unknown Market flag"},
    {{0x2261, bm2bp(0x20)}, "Armor Polish"},
    {{0x2261, bm2bp(0x40)}, "Chocobo Egg"},
    {{0x2261, bm2bp(0x80)}, "Insect Incense"},
    {{0x2262, bm2bp(0x01)}, "Jade Disk"},
    {{0x2262, bm2bp(0x02)}, "Jaguar Ring"},
    {{0x2262, bm2bp(0x04)}, "Magic Gourd"},
    {{0x2262, bm2bp(0x08)}, "Moxa Stick"},
    {{0x2262, bm2bp(0x10)}, "Oracle Bone"},
    {{0x2262, bm2bp(0x20)}, "Ruby Heart"},
    {{0x2262, bm2bp(0x40)}, "Silver Sheath"},
    {{0x2262, bm2bp(0x80)}, "Staff of Life"},
    {{0x2263, bm2bp(0x01)}, "Sun Stone"},
    {{0x2263, bm2bp(0x02)}, "Thug's Cloak"},
    {{0x2263, bm2bp(0x04)}, "Wizard's Coin"},
    {{0x2263, bm2bp(0x08)}, "Unknown Market flag"},
    {{0x2263, bm2bp(0x10)}, "Unknown Market flag"},
    {{0x2263, bm2bp(0x20)}, "Unknown Market flag"},
    {{0x2263, bm2bp(0x40)}, "Unknown Market flag"},
    {{0x2263, bm2bp(0x80)}, "Unknown Market flag"},
    {{0x2264, bm2bp(0x01)}, "Diamond Eye"},
    {{0x2264, bm2bp(0x02)}, "Diamond Eyes"},
    {{0x2264, bm2bp(0x04)}, "Gauge"},
    {{0x2264, bm2bp(0x08)}, "Wheel"},
    {{0x2264, bm2bp(0x10)}, "Queen's Key"},
    {{0x2264, bm2bp(0x20)}, "Energy Core"},
    {{0x2264, bm2bp(0x40)}, "Gourd in Act1 Huts"},
    {{0x2264, bm2bp(0x80)}, "Gourd in Act1 Huts"},
    {{0x2265, bm2bp(0x01)}, "Gourd in Act1 Huts"},
    {{0x2265, bm2bp(0x02)}, "Gourd in Act1 Huts"},
    {{0x2265, bm2bp(0x04)}, "Gourd in Act1 Huts"},
    {{0x2265, bm2bp(0x08)}, "Gourd in Act1 Huts"},
    {{0x2265, bm2bp(0x10)}, "Gourd in Act1 Huts"},
    {{0x2265, bm2bp(0x20)}, "Gourd in Act1 Huts"},
    {{0x2265, bm2bp(0x40)}, "Gourd in Act1 Huts"},
    {{0x2265, bm2bp(0x80)}, "Gourd in Act1 Huts"},
    {{0x2266, bm2bp(0x01)}, "Gourd in Act1 Huts"},
    {{0x2266, bm2bp(0x02)}, "Gourd in Act1 Huts"},
    {{0x2266, bm2bp(0x04)}, "Gourd in Act1 Huts"},
    {{0x2266, bm2bp(0x08)}, "Gourd in Act1 Huts"},
    {{0x2266, bm2bp(0x10)}, "???"},
    {{0x2266, bm2bp(0x20)}, "Gourd in Act1 Huts"},
    {{0x2266, bm2bp(0x40)}, "Gourd in Act1 Huts"},
    {{0x2266, bm2bp(0x80)}, "Gourd in Act1 Huts (w/ changing content)"},
    {{0x2267, bm2bp(0x01)}, "Gourd in Act1 Huts"},
    {{0x2267, bm2bp(0x02)}, "Gourd in Act1 Huts"},
    {{0x2267, bm2bp(0x04)}, "Gourd in Act1 Huts"},
    {{0x2267, bm2bp(0x08)}, "Gourd in Act1 Huts"},
    {{0x2267, bm2bp(0x10)}, "Gourd in Act1 Huts"},
    {{0x2267, bm2bp(0x20)}, "Gourd in Act1 Huts"},
    {{0x2267, bm2bp(0x40)}, "Gourd in Act1 Huts"},
    {{0x2267, bm2bp(0x80)}, "???"},
    {{0x2268, bm2bp(0x01)}, "Gourd in Act1 Huts"},
    {{0x2268, bm2bp(0x02)}, "Gourd near Defend (w/ changing content)"},
    {{0x2268, bm2bp(0x04)}, "Gourd near Defend"},
    {{0x2268, bm2bp(0x08)}, "Gourd near Defend"},
    {{0x2268, bm2bp(0x10)}, "Gourd in south Jungle"},
    {{0x2268, bm2bp(0x20)}, "Gourd in south Jungle"},
    {{0x2268, bm2bp(0x40)}, "Gourd in south Jungle"},
    {{0x2268, bm2bp(0x80)}, "Gourd in south Jungle"},
    
    {{0x2273, bm2bp(0x20)}, "top of volcano visited"},
    
    {{0x2281, bm2bp(0x40)}, "Honey gourd below 'mids'"},
    {{0x2281, bm2bp(0x80)}, "Herbal gourd below 'mids'"},
    {{0x2282, bm2bp(0x01)}, "Pixie dust gourd below 'mids'"},
    {{0x2282, bm2bp(0x02)}, "Ethanol gourd below 'mids'"},
    {{0x2282, bm2bp(0x04)}, "Wings gourd below 'mids'"},
    {{0x2282, bm2bp(0x08)}, "Call bead gourd below 'mids'"},
    {{0x2282, bm2bp(0x10)}, "Biscuit gourd below 'mids'"},
    
    {{0x2287, bm2bp(0x20)}, "Opened Laser Lance Gourd"},
    {{0x2288, bm2bp(0x04)}, "Received Petal or Honey in FE Village Huts"},
    {{0x2288, bm2bp(0x08)}, "Talked to defend guy"},
    {{0x228a, bm2bp(0x40)}, "Ruins SE switch activated"},
    {{0x228b, bm2bp(0x01)}, "FE visited pre-thraxx (East exit check)"},
    
    {{0x22c6, bm2bp(0x01)}, "(Unset) infinite callbead flag"},
    
    {{0x22d8, bm2bp(0x40)}, "Diamond Eye 'mids"},
    {{0x22d8, bm2bp(0x80)}, "Diamond Eye Halls"},
    {{0x22d9, bm2bp(0x01)}, "Horace met/Madronius spawned"},
    {{0x22d9, bm2bp(0x08)}, "Aegis dead"},
    
    {{0x22d9, bm2bp(0x20)}, "Diamond Eyes stolen"},
    
    {{0x22da, bm2bp(0x01)}, "Unused?"}, // TODO: test more. seems to do nothing
    {{0x22da, bm2bp(0x02)}, "Bone Crusher"},
    {{0x22da, bm2bp(0x04)}, "Gladiator Sword"},
    {{0x22da, bm2bp(0x08)}, "Crusader Sword"},
    {{0x22da, bm2bp(0x10)}, "Neutron Blade"},
    {{0x22da, bm2bp(0x20)}, "Spider's Claw"},
    {{0x22da, bm2bp(0x40)}, "Bronze Axe"},
    {{0x22da, bm2bp(0x80)}, "Knight Basher"},
    {{0x22db, bm2bp(0x01)}, "Atom Smasher"},
    {{0x22db, bm2bp(0x02)}, "Horn Spear"},
    {{0x22db, bm2bp(0x04)}, "Bronze Spear"},
    {{0x22db, bm2bp(0x08)}, "Lance"},
    {{0x22db, bm2bp(0x10)}, "Laser Lance"},
    {{0x22db, bm2bp(0x20)}, "Bazooka"},
    
    {{0x22db, bm2bp(0x40)}, "Horace's Aura"},
    {{0x22db, bm2bp(0x80)}, "Horace's Regenerate"},
    
    {{0x22dc, bm2bp(0x01)}, "Pigrace finished"},
    {{0x22dc, bm2bp(0x04)}, "Got Cecil's Bazooka?"},
    {{0x22dc, bm2bp(0x08)}, "windwalker unlocked"},
    {{0x22dc, bm2bp(0x10)}, "Gave away Gauge"},
    {{0x22dc, bm2bp(0x20)}, "Gave away Wheel"},
    {{0x22dc, bm2bp(0x40)}, "Gave away Diamond Eyes"},
    
    {{0x22dd, bm2bp(0x01)}, "Verminator dead"},
    {{0x22dd, bm2bp(0x02)}, "Sterling dead"},
    {{0x22dd, bm2bp(0x04)}, "Timberdrake dead"},
    {{0x22dd, bm2bp(0x40)}, "Load east castle"},
    
    {{0x22de, bm2bp(0x04)}, "Queen cutscene below chessboard watched"},
    {{0x22de, bm2bp(0x80)}, "Talked to Cecil"},
    
    {{0x22df, bm2bp(0x01)}, "Doubles dead"},
    {{0x22df, bm2bp(0x02)}, "Crustacia elevator in top position"},
    {{0x22df, bm2bp(0x10)}, "Crustacia rock thrown"},
    {{0x22df, bm2bp(0x20)}, "Crustacia rock landed"},
    {{0x22df, bm2bp(0x80)}, "Cave raptors killed"},
    
    {{0x22e2, bm2bp(0x01)}, "Some flag in 'mids bottom"},
    {{0x22e2, bm2bp(0x02)}, "Some flag in 'mids bottom"},
    {{0x22e2, bm2bp(0x04)}, "Some flag in 'mids bottom"},
    {{0x22e2, bm2bp(0x08)}, "Some flag in 'mids bottom"},
    {{0x22e2, bm2bp(0x10)}, "Some flag in 'mids top"},
    {{0x22e2, bm2bp(0x20)}, "Some flag in 'mids top"},
    {{0x22e2, bm2bp(0x40)}, "Sons defeated in 'mids"},
    {{0x22e2, bm2bp(0x80)}, "Unknown flag checked below 'mids. Levitated?"},
    
    {{0x22e3, bm2bp(0x01)}, "Unknown flag checked below 'mids. Levitated?"},
    {{0x22e3, bm2bp(0x02)}, "Wall below 'mids broken"},
    {{0x22e3, bm2bp(0x04)}, "Wall below 'mids broken"},
    {{0x22e3, bm2bp(0x08)}, "Wall below 'mids broken"},
    {{0x22e3, bm2bp(0x10)}, "Wall below 'mids broken"},
    {{0x22e3, bm2bp(0x20)}, "Tiny defeated"},
    {{0x22e3, bm2bp(0x40)}, "Unknown flag checked out and inside 'mids. Dog freed?"},
    {{0x22e3, bm2bp(0x80)}, "Unknown flag used outside 'mids"},
    
    {{0x22e5, bm2bp(0x01)}, "Unknown 'mids flag"},
    {{0x22e5, bm2bp(0x02)}, "Unknown 'mids flag"},
    {{0x22e5, bm2bp(0x04)}, "Unknown 'mids flag"},
    {{0x22e5, bm2bp(0x08)}, "WW Landing (set before loading fire pit from OW)"},
    {{0x22e5, bm2bp(0x10)}, "???"},
    {{0x22e5, bm2bp(0x20)}, "Defated raptors?"}, // FE visited pre-thraxx or post-ww??
    {{0x22e5, bm2bp(0x40)}, "West castle collapsed"},
    {{0x22e5, bm2bp(0x80)}, "???"}, // USED in hundo
    
    {{0x22e6, bm2bp(0x01)}, "Footknight defeated"},
    {{0x22e6, bm2bp(0x20)}, "Light in storage room"},
    
    {{0x22e7, bm2bp(0x80)}, "Talked to Dude below chessboard"},
    {{0x22e8, bm2bp(0x02)}, "Salabog fight started"},
    {{0x22e9, bm2bp(0x40)}, "Ruins Minitaur defeated"},
    
    {{0x22ea, bm2bp(0x01)}, "Loot successful"},
    
    {{0x22ea, bm2bp(0x04)}, "No previous save found"},
    
    {{0x22eb, bm2bp(0x02)}, "start pressed in intro"},
    {{0x22eb, bm2bp(0x04)}, "running showcase"},
    {{0x22eb, bm2bp(0x20)}, "in animation"},
    {{0x22ec, bm2bp(0x04)}, "on bug legs"},
    {{0x22ec, bm2bp(0x08)}, "stepped on geyser"},
    {{0x22ec, bm2bp(0x20)}, "unknown intro/outro? flag in prof. lab"},
    //{{0x22ed, bm2bp(0x04)}, "Raptors defeated"}, // this seems to be a temp for raptors
    {{0x22ed, bm2bp(0x08)}, "crustacia intro to be shown"},
    {{0x22ed, bm2bp(0x80)}, "Falling into a pit"},
    {{0x22ee, bm2bp(0x01)}, "unknown intro/outro? flag in prof. lab"},
    
    {{0x22ef, bm2bp(0x02)}, "Square-reminder message in market shown"},
    
    {{0x22f0, bm2bp(0x20)}, "Received grass vest in FE village"},
    {{0x22f1, bm2bp(0x02)}, "Stealing from gourds is OK msg shown"},
    {{0x22f1, bm2bp(0x20)}, "Also FE visited pre-thraxx or post-ww??"},
    {{0x22f1, bm2bp(0x40)}, "Inside outro?"},
    {{0x22f2, bm2bp(0x01)}, "In creadits"},
    {{0x22f2, bm2bp(0x02)}, "Swamp leafpad activated"},
    {{0x22f2, bm2bp(0x40)}, "Horace met past aegis/ww?"},
    {{0x22f2, bm2bp(0x80)}, "Palace guard alt. dialog/easter egg"},
    {{0x22f3, bm2bp(0x01)}, "Crush dialog to be shown"},
    {{0x22f3, bm2bp(0x08)}, "Coming from north end"},
    {{0x22f3, bm2bp(0x20)}, "Salabog defeated? invincible?"},
    {{0x22f4, bm2bp(0x08)}, "Prison door 1 open"},
    {{0x22f4, bm2bp(0x10)}, "Prison door 2 open"},
    {{0x22f4, bm2bp(0x20)}, "Prison door 3 open"},
    {{0x22f4, bm2bp(0x40)}, "Prison door 4 open"},
    {{0x22f4, bm2bp(0x80)}, "Prison door 5 open"},
    {{0x22f5, bm2bp(0x01)}, "Prison door 6 open"},
    {{0x22f5, bm2bp(0x02)}, "Prison door 7 open"},
    {{0x22f5, bm2bp(0x80)}, "Naris' age?"},
    
    {{0x22f9, bm2bp(0x10)}, "Second half of Lab cutscene to be played"},
    {{0x22f9, bm2bp(0x20)}, "Energy core teleporter used?"},
    
    // -- auto-generated list of sniffing flags, if we'd parse the rom twice we could skip this definition --
#ifdef __has_include
#if __has_include("sniffflags.inc") // optional include
#include "sniffflags.inc"
#endif
#else // user has to create an empty file
#include "sniffflags.inc"
#endif
    // -- --
};



enum class IntType : uint8_t { none=0, word, byte, subinstr1B, subinstr2B, subinstr3B };

struct DoggoData;
struct DoggoData {
    IntType inttype=IntType::none;
    uint16_t val=0;
    uint8_t mapid=0;
    DoggoData() {}
    DoggoData(IntType i, uint16_t v, uint8_t m) : inttype(i) , val(v) , mapid(m) {}
};

struct LootData;
struct LootData {
    enum class DataSet : uint8_t { none=0, item=1, amount=2, extra=4, check=8, set=16, callSniff=32, callGourd=64 };
    DataSet dataset=DataSet::none;
    uint8_t __padding=0;
    uint16_t amount=0;
    uint16_t item=0;
    uint16_t extra=0;
    uint32_t amount_pos=0;
    uint32_t item_pos=0;
    uint32_t extra_pos=0;
    IntType amount_type=IntType::none;
    IntType item_type=IntType::none;
    IntType extra_type=IntType::none;
    std::pair<uint16_t, uint8_t> check_flag={0,0};
    std::pair<uint16_t, uint8_t> set_flag={0,0};
    uint8_t mapid=0;
    uint16_t scriptid=0;
    uint8_t x1=0;
    uint8_t y1=0;
    uint8_t x2=0;
    uint8_t y2=0;
    uint16_t mapref=0;
    LootData(){}
    LootData(uint8_t mapid, uint16_t scriptid, uint8_t x1, uint8_t y1, uint8_t x2, uint8_t y2) {
        this->scriptid=scriptid; this->mapid=mapid; this->x1=x1; this->y1=y1; this->x2=x2; this->y2=y2;
    }
    std::string to_string() const;
    std::string to_flag() const;
    std::string to_h() const;
    inline bool same_source(const LootData& other) const {
        return ( amount_pos==other.amount_pos &&
                 item_pos==other.item_pos &&
                 extra_pos==other.extra_pos );
    }
};

inline LootData::DataSet operator| (LootData::DataSet lhs, LootData::DataSet rhs) {
    using T = std::underlying_type <LootData::DataSet>::type;
    // using T = std::underlying_type_t <LootData::DataSet>; // c++14
    return static_cast<LootData::DataSet>(static_cast<T>(lhs) | static_cast<T>(rhs));
};
inline LootData::DataSet& operator|= (LootData::DataSet& lhs, LootData::DataSet rhs) { lhs = lhs | rhs; return lhs; };
inline LootData::DataSet operator& (const LootData::DataSet lhs, LootData::DataSet rhs) {
    using T = std::underlying_type <LootData::DataSet>::type;
    // using T = std::underlying_type_t <LootData::DataSet>; // c++14
    return static_cast<LootData::DataSet>(static_cast<T>(lhs) & static_cast<T>(rhs));
};
inline LootData::DataSet& operator&= (LootData::DataSet& lhs, LootData::DataSet rhs) { lhs = lhs & rhs; return lhs; };
inline bool operator! (LootData::DataSet e) { return e==LootData::DataSet::none; }

const char* to_string(IntType t) {
    using T=IntType;
    switch (t) { 
        case T::byte: return "Byte";
        case T::word: return "Word";
        case T::subinstr1B: return "Sub1B";
        case T::subinstr2B: return "Sub2B";
        case T::subinstr3B: return "Sub3B";
        case T::none:
        default: return "-";
    }
}
std::string LootData::to_string() const {
    char buf[256];
    if (!!(dataset & LootData::DataSet::amount))
    snprintf(buf, sizeof(buf), "map 0x%02x:%04x@%02x,%02x:%02x,%02x: %3ux 0x%04x (%-5s) @$%06x, next+%u, check $%04x&0x%02x, set $%04x&0x%02x",
        mapid, scriptid, x1, y1, x2, y2, amount, item, ::to_string(item_type), item_pos, extra,
        check_flag.first, (1<<check_flag.second),
        set_flag.first, (1<<set_flag.second));
    else
    snprintf(buf, sizeof(buf), "map 0x%02x:%04x@%02x,%02x:%02x,%02x:      0x%04x (%-5s) @$%06x, next+%u, check $%04x&0x%02x, set $%04x&0x%02x",
        mapid, scriptid, x1, y1, x2, y2, item, ::to_string(item_type), item_pos, extra,
        check_flag.first, (1<<check_flag.second),
        set_flag.first, (1<<set_flag.second));
    return std::string(buf);
}
std::string LootData::to_flag() const {
    char buf[256];
    char sitem[32]; snprintf(sitem, sizeof(sitem), "0x%04x ", item);
    char umap[16]; snprintf(umap, sizeof(umap), "MAP 0x%02x", mapid);
    const char* smap = umap;
    
    for (auto pair: maps) {
        if (pair.first == mapid) {
            smap = pair.second;
            const char* tmp = strstr(smap, "- ");
            if (tmp) smap = tmp+2;
            break;
        }
    }
    
    const auto& prizeit = prizes.find(item);
    if (prizeit != prizes.end()) {
        strncpy(sitem, prizeit->second, sizeof(sitem));
        sitem[sizeof(sitem)-1] = 0;
        char* p = strchr(sitem, '(');
        if (p) *p = 0;
    }
    
    snprintf(buf, sizeof(buf), "{{0x%04x, bm2bp(0x%02x)}, \"Sniffed %sin %s (#%u)\"},",
        check_flag.first, (1<<check_flag.second), sitem, smap, (unsigned)mapref);
    return std::string(buf);
}
std::string LootData::to_h() const {
    // NOTE: since all sniff values are 16bit, we only dump addr+value
    char buf[64];
    snprintf(buf, sizeof(buf), "{0x%06x, 0x%04x},", item_pos&~0xc00000, item);
    return std::string(buf);
}



static inline bool addr_valid(uint32_t addr, size_t len)
{
    return ((addr&~(0xc00000)) < len);
}
static uint8_t read_buf8(const uint8_t* buf, uint32_t addr, size_t len)
{
    addr &= ~(0xc00000);
    if (addr >= len) {
        fprintf(stderr, "** Illegal address 0x%x read **\n", (unsigned)addr);
        assert(false);
        return 0;
    }
    return buf[addr];
}
static uint16_t read_buf16(const uint8_t* buf, uint32_t addr, size_t len)
{
    addr &= ~(0xc00000);
    if (addr+1 >= len) {
        fprintf(stderr, "** Illegal address 0x%x read **\n", (unsigned)addr);
        assert(false);
        return 0;
    }
    uint16_t res = 0;
    res |= buf[addr+1]; res<<=8;
    res |= buf[addr];
    return res;
}
static uint32_t read_buf24(const uint8_t* buf, uint32_t addr, size_t len)
{
    addr &= ~(0xc00000);
    if (addr+2 >= len) {
        fprintf(stderr, "** Illegal address 0x%x read **\n", (unsigned)addr);
        assert(false);
        return 0;
    }
    uint32_t res = 0;
    res |= buf[addr+2]; res<<=8;
    res |= buf[addr+1]; res<<=8;
    res |= buf[addr];
    return res;
}

#define addr_valid(addr) addr_valid(addr, len)
#define read8(addr)  read_buf8 (buf, addr, len)
#define read16(addr) read_buf16(buf, addr, len)
#define read24(addr) read_buf24(buf, addr, len)

static inline uint32_t rom2scriptaddr(uint32_t romaddr)
{
    romaddr &= ~(0x8000);
    romaddr -= 0x920000;
    return (romaddr&0x007fff) + ((romaddr&0x1ff0000)>>1);
}

static inline uint32_t script2romaddr(uint32_t scriptaddr)
{
    return 0x928000 + (scriptaddr&0x007fff) + ((scriptaddr&0xff8000)<<1);
}

static bool sub1bIsVal(uint8_t subinstr)
{
    uint8_t cmd = subinstr&0x70;
    return (cmd==0x30 || cmd==0x40 || cmd==0x60);
}
static bool sub1bIsFinalVal(uint8_t subinstr)
{
    return ((subinstr&0x80) && sub1bIsVal(subinstr));
}
static uint16_t sub1b2val(uint8_t subinstr)
{
    uint8_t cmd = subinstr&0x70;
    if (cmd == 0x30) return subinstr&0x0f;
    if (cmd == 0x40) return 0xfff0|(subinstr&0x0f);
    if (cmd == 0x60) return 0x10 + (subinstr&0x0f);
    fprintf(stderr, "WARN: Sub-instr %02x is not a value!\n", subinstr);
    return 0;
}
static int16_t sub1b2sval(uint8_t subinstr)
{
    return (int16_t)sub1b2val(subinstr);
}
static std::string sub2currency(const std::string& subresult)
{
    std::string res;
    char* next = nullptr;
    long curval = strtol(subresult.c_str(), &next, 0);
    bool isint = (next && !*next);
    if (isint && curval == 0x00) res = "Talons";
    else if (isint && curval == 0x03) res = "Jewels";
    else if (isint && curval == 0x06) res = "Gold Coins";
    else if (isint && curval == 0x09) res = "Credits";
    else res = "Currency "+subresult;
    return res;
}
static bool subinstrIsEntityOnly(uint8_t subinstr)
{
    switch (subinstr) { // NOTE: this excludes calculations
        case 0xd0: // boy
        case 0xd1: // dog
        case 0xd2: // controlled char
        case 0xd3: // non-controlled char
        case 0xad: // last entity ($0341)
        case 0xae: // entity attached to script?
            return true;
        default:
            return false;
    }
}
static bool subinstrIsEntity(uint8_t subinstr)
{
    return subinstrIsEntityOnly(subinstr|0x80);
}
static const char* subinstr2name(uint8_t subinstr)
{
    switch (subinstr&0x7f) {
        case 0x50:
            return "boy";
        case 0x51:
            return "dog";
        case 0x52:
            return "controlled char";
        case 0x53:
            return "non-controlled char";
        case 0x2d:
            return "last entity ($0341)";
        case 0x2e:
            return "entity attached to script?";
        default:
            return "???";
    }
}

static const char* absscript2name(uint32_t addr)
{
    for (const auto& pair: absscripts) {
        if (pair.first == addr)
            return pair.second;
    }
    return "Unknown";
}
static const char* npcscript2name(uint16_t id)
{
    for (const auto& pair: npcscripts) {
        if (pair.first == id)
            return pair.second;
    }
    return "Unknown";
}
static const char* globalscript2name(uint8_t id, const char* def="Unknown")
{
    for (const auto& pair: globalscripts) {
        if (pair.first == id) {
            return pair.second;
        }
    }
    return def;
}
   
static std::string ramaddr2str(uint16_t addr)
{
    auto it = ram.find(addr);
    if (it != ram.end()) return it->second;
    
    char buf[6];
    snprintf(buf, 6, "$%04x", addr);
    return std::string(buf);
}
static std::string rambit2str(uint16_t addr, uint8_t bp)
{
    // TODO: read from a map
    // bp2bm: 0:0x01, 1:0x02, 2:0x04, 3:0x08, 4:0x10, 5:0x20, 6:0x40, 7:0x80
    auto it = flags.find(std::make_pair(addr,bp));
    if (it != flags.end())
        return std::string("(") + it->second + ") ";
    return "";
}
static std::string hex(uint32_t val)
{
    char buf[64]; snprintf(buf, sizeof(buf), "%02x", (unsigned)val);
    return std::string(buf);
}
static std::string hex6(uint32_t val)
{
    char buf[64]; snprintf(buf, sizeof(buf), "%06x", (unsigned)val);
    return std::string(buf);
}
static std::string buf_get_text(const uint8_t* buf, uint32_t addr, uint32_t len, const char* spaces1="", const char* spaces2="")
{
    std::string data = "";
    std::string info = "";
    std::string linebreak = std::string("\"\n") + spaces1 + spaces2 + "\"";
    addr = read24(addr);
    bool mode = (addr & 0x800000);
    addr = 0xc00000 + (addr & 0x7fff) + ((addr & 0x7f8000)<<1);
    if (addr>0xcfffff) return "";
    #if 1
    //out += mode?"1,":"0,";
    info += hex6(addr);
    info += GT " ";
    #endif
    
    if (mode) {
        uint8_t next_plain = 0;
        do {
            uint8_t d = read8(addr);
            if (next_plain) {
                next_plain--;
                if (!d && !next_plain) break;
                data += (char)d;
            } else if (d==0xc0) {
                addr++;
#ifdef TEXT_SHOW_DICT_USE
                data += '(';
                data += hex(read8(addr));
                data += ')';
#endif
                uint32_t wordpp = 0x91f46c + ((uint16_t)read8(addr)<<1);
                uint32_t wordp = 0x91f7d5 + read16(wordpp);
                for (char c; (c=read8(wordp)); wordp++)  {
                    data += c;
                }
            } else if ((d&0xc0)==0xc0) {
#ifdef TEXT_SHOW_DICT_USE
                data += '(';
                data += hex(d);
                data += ')';
#endif
                d = (d<<1)&0x7e;
                uint32_t wordpp = 0x91f3ec + d;
                uint32_t wordp = 0x91f66c + read16(wordpp);
                for (char c; (c=read8(wordp)); wordp++)  {
                    data += c;
                }
            } else if ((d&0xc0)==0x40) {
                next_plain = d&0x3f;
            } else if ((d&0xc0)==0x80) {
                d<<=1;
                char c;
                c = (char)read8(0x91f32e + d);
                data += c;
                c = (char)read8(0x91f32f + d);
                if (!c) break;
                data += c;
            } else if ((d&0xc0)==0) {
                char c = (char)read8(0x91f3ae + d);
                if (!c) break;
                data += c;
            }
            addr++;
        } while (true);
    } else {
        for (char c; (c=read8(addr)); addr++)  {
            data += c;
        }
    }
    
    // make raw data look nice
    std::string printable = "\"";
    char c1=0, c2=0;
    size_t n=0, m=0;
    bool inPause=false;
    for (auto c: data) {
        if (m>0 && ((uint8_t)c == 0x96 || (uint8_t)c == 0x97)) {
            printable += "\"\n\"";
            n=0; m=0;
        }

        if ((uint8_t)c==0x80 && (uint8_t)c2==0x80 && inPause) {
            // c1 = duration?
            if (c1>=0x20 && c1<0x7f)
                printable.erase(printable.length()-7, 7);
            else
                printable.erase(printable.length()-12, 12);
            #if 1
            char buf[3]; snprintf(buf, sizeof(buf), "%02x", (uint8_t)c1);
            printable+="[PAUSE:" + std::string(buf) + "]";
            m += 6+3;
            #else
            printable+="[PAUSE]";
            m += 6;
            #endif
        }
        else if (c>=0x20 && c<0x7f) printable+=c;
        else if (c=='\n') { printable+="[LF]"; m+=3; }
        else { printable += "[0x" + hex((uint8_t)c) + "]"; m+=5; }

        if ((uint8_t)c==0x80) inPause=!inPause;
        n++; m++;
        if (((c==' ' || (!inPause && (uint8_t)c==0x80)) && (n>=80 || m>=80)) || c=='\n') {
            printable += linebreak;
            n=0; m=0;
        }
        c2 = c1;
        c1 = c;
    }
    printable += '"';
    
    return info+printable;
}
#define get_text(addr) buf_get_text(buf, addr, len)
#define get_text_i(addr, spaces1, spaces2) buf_get_text(buf, addr, len, spaces1, spaces2)

static std::string u16addr2str(uint16_t n)
{
    char buf[6];
    snprintf(buf, sizeof(buf), "$%04x", n);
    return buf;
}
static std::string u8val2str(uint8_t n)
{
    char buf[5];
    snprintf(buf, sizeof(buf), "0x%02x", n);
    return buf;
}
static std::string u16val2str(uint16_t n)
{
    char buf[7];
    snprintf(buf, sizeof(buf), "0x%04x", n);
    return buf;
}
static std::string u24val2str(uint32_t n)
{
    char buf[11];
    snprintf(buf, sizeof(buf), "0x%06x", n);
    return buf;
}

static std::string buf_parse_sub(const uint8_t* buf, uint32_t& addr, size_t len, bool* pok=nullptr, int* pexprlen=nullptr)
{
    bool ok = true;
    bool done = false;
    std::string res;

    uint8_t instr = 0;
    int exprlen = 0;
    static std::stack< std::pair<int,std::string> > stack; // yes, static...
    do {
        instr = read8(addr++);
        done = instr&0x80;
        if (subinstrIsEntity(instr)) {
            exprlen++;
            if (!res.empty()) res += " ";
            res += subinstr2name(instr);
        } else if (sub1bIsVal(instr)) {
            exprlen++;
            if (!res.empty()) res += " ";
            res += std::to_string((int)sub1b2sval(instr)); // signed decimal for now
        } else {
            instr &= 0x7f;
            switch(instr) {
            case 0x00: // noop?
                break;
            case 0x01: // signed const byte
            case 0x02: // unsigned const byte
            {
                bool issigned = (instr==0x01);
                exprlen++;
                if (!res.empty()) res += " ";
                res += u8val2str(read8(addr++));
                if (issigned) res += " signed";
                break;
            }
            case 0x03: // signed const word
            case 0x04: // unsigned const word
            {
                bool issigned = (instr==0x03);
                exprlen++;
                if (!res.empty()) res += " ";
                res += u16val2str(read16(addr)); addr+=2;
                if (issigned) res += " signed";
                break;
            }
            case 0x05: // test bit
            case 0x0a: // test temp bit
            {
                uint16_t baseaddr = (instr>=0x0a) ? 0x2834 : 0x2258;
                uint8_t a = read16(addr)>>3;
                uint8_t b = read16(addr)&0x07;
                addr+=2;
                res += u16addr2str(baseaddr+a) + "&" + u8val2str(1<<b); // TODO: named bits
                exprlen=2;
                break;
            }
            case 0x06: // read byte, signed
            case 0x07: // read byte, unsigned
            case 0x08: // read word, signed
            case 0x09: // read word, unsigned
            case 0x0b: // read temp byte, signed
            case 0x0c: // read temp byte, unsigned
            case 0x0d: // read temp word, signed
            case 0x0e: // read temp word, unsigned
            {
                bool isbyte = (instr==0x06 || instr==0x07 || instr==0x0b || instr==0x0c);
                //bool issigned = (instr==0x06 || instr==0x08 || instr==0x0b || instr==0x0d);
                uint16_t baseaddr = (instr>=0x0a) ? 0x2834 : 0x2258;
                exprlen++;
                if (!res.empty()) res += " ";
                res = u16addr2str(baseaddr+read16(addr)); // TODO: named vars
                addr+=2;
                if (isbyte) {
                    res = "("+res+")&0xff";
                    exprlen=2;
                }
                break;
            }
            case 0x0f: // script arg bit, like 05 and 08 but addr is only 8bit
            {
                uint8_t a = read8(addr)>>3;
                uint8_t b = read8(addr++)&0x07;
                if (!res.empty()) res += " ";
                res += "arg" + std::to_string(a) + "&" + u8val2str(1<<b);
                exprlen=2;
                break;
            }
            case 0x10: // signed byte script arg
            case 0x11: // unsigned byte script arg
            case 0x12: // signed word script arg
            case 0x13: // unsigned word script arg
            {
                bool isbyte = (instr==0x10 || instr==0x11);
                bool issigned = (instr==0x10 || instr==0x12);
                exprlen++;
                if (!res.empty()) res += " ";
                res += (issigned ? "signed arg" : "arg") + std::to_string(read8(addr++));
                if (isbyte) {
                    res += "&0xff";
                    exprlen=2;
                }
                break;
            }
            case 0x14: // boolean invert
            case 0x15: // bitwise invert
            case 0x16: // flip sign
            {
                const char* op = (instr==0x14) ? "!" : (instr==0x15) ? "~" : "-";
                std::string b = (exprlen>1) ? ("("+res+")") : res;
                res = op+b;
                exprlen = 2;
                break;
            }
            case 0x17: // pull from stack, res = pulled * res
            case 0x18: // pulled / res
            case 0x1a: // pulled + res
            case 0x1b: // pulled - res
            case 0x1c: // pulled << res
            case 0x1d: // pulled >> res
            case 0x1e: // pulled < res (signed)
            case 0x1f: // pulled > res (signed)
            case 0x20: // pulled <= res (signed)
            case 0x21: // pulled >= res (signed)
            case 0x22: // pulled == res
            case 0x23: // pulled != res
            case 0x24: // pulled & res
            case 0x25: // pulled | res
            case 0x26: // pulled ^ res
            case 0x27: // pulled || res
            case 0x28: // pulled && res
            {
                const char* op = (instr==0x17) ? " * " : (instr==0x18) ? " / " :
                                 (instr==0x1a) ? " + " : (instr==0x1b) ? " - " :
                                 (instr==0x1c) ? LT LT : (instr==0x1d) ? GT GT :
                                 (instr==0x1e) ? " " LT " " : (instr==0x1f) ? " " GT " " :
                                 (instr==0x20) ? " " LE " " : (instr==0x21) ? " " GE " " :
                                 (instr==0x22) ? " == "     : (instr==0x23) ? " != "     :
                                 (instr==0x24) ? " & " : (instr==0x25) ? " | " :
                                 (instr==0x26) ? " ^ " : (instr==0x27) ? " || " : " && ";
                if (stack.empty()) {
                    fprintf(stderr, "WARN: pulled from empty stack at $%06x!!\n", addr);
                    ok = false;
                    break; // disable for debugging
                }
                auto& pulled = stack.top();
                std::string a = (pulled.first>1) ? ("("+pulled.second+")") : pulled.second;
                std::string b = (exprlen>1) ? ("("+res+")") : res;
                res = a + op + b;
                exprlen = 2;
                stack.pop();
                break;
            }
            case 0x29: // push to stack
                stack.push(std::make_pair(exprlen,std::move(res)));
                exprlen=0;
                break;
            case 0x2a: // random word
                exprlen++;
                if (!res.empty()) res += " ";
                res += "RAND";
                break;
            case 0x2b: // (random word * $2) >> 16 = randrange[0,$2[
                res = "RANDRANGE(0," LT + res + ")";
                exprlen=1;
                break;
            case 0x2c: // dialog response
                exprlen=1;
                if (res.empty())
                    res = "Dialog response";
                else {
                    res = "Dialog response (preselect "+res+")";
                    exprlen++;
                }
                break;
            case 0x54: // $2 = script data[0x09]
                if (!res.empty()) res += " ";
                exprlen++;
                res += "script[0x9]";
                break;
            case 0x55: // deref res
            case 0x56: // deref res &0xff
                if (exprlen>1)
                    res = "*(" + res + ")";
                else
                    res = "*" + res;
                exprlen = 1;
                if (instr==0x56) {
                    res = "("+res+")&0xff";
                    exprlen=2;
                }
                break;
            case 0x57:
                if (!res.empty()) res += " ";
                exprlen++;
                res += "(player==dog)";
                break;
            case 0x58: // game timer bits 0-15 ($7e0b19..7e0b1a)
                if (!res.empty()) res += " ";
                exprlen+=2;
                res += "GameTimer&0xffff";
                break;
            case 0x59: // bits 16-32 ($7e0b1b..7e0b1c)
                if (!res.empty()) res += " ";
                exprlen+=2;
                res += "GameTimer>>16";
                break;
            case 0x5a: // Run shop: buy, get result
                if (!res.empty()) res += " ";
                exprlen++;
                res += "Shop buy result";
                break;
            case 0x5b: // sell
                if (!res.empty()) res += " ";
                exprlen++;
                res += "Shop sell result";
                break;
            case 0x5c: // Next damage will kill entity
                if (exprlen>1)
                    res = "(" + res + ") will die";
                else
                    res = res + " will die";
                exprlen = 2;
                break;
            case 0x51:
            case 0x19:
            case 0x2f:
            case 0x5d:
            case 0x5e:
            case 0x5f:
                fprintf(stderr, "WARN: Invalid sub-instr 0x%02x at $%06x!\n", instr, addr);
                if (!res.empty()) res += " ";
                res += "[invalid " + u8val2str(instr) + "]";
                ok = false;
                break;
            default:
                fprintf(stderr, "WARN: Unhandled sub-instr 0x%02x at $%06x!\n", instr, addr);
                if (!res.empty()) res += " ";
                res += "[unknown " + u8val2str(instr) + "]";
                ok = false;
            }
        }
    } while (ok && !done);

    if (pok) *pok = ok;
    if (pexprlen) *pexprlen = exprlen;
    if (ok && !stack.empty()) fprintf(stderr, "WARN: sub-instr stack not empty!\n"); // sadly this is actually used by the devs :S
    else if (!ok) stack = std::stack< std::pair<int,std::string> >();
    return res;
}
#define parse_sub(addrRef, ...) buf_parse_sub(buf, addrRef, len, __VA_ARGS__)

static void printwrite(const char* spaces, unsigned instroff, uint8_t instr, uint16_t ramaddr, uint16_t val, const char* hex="") // this is loRAM only
{
    const char* addrname = nullptr;
    const char* valname = nullptr;

    auto ramit = ram.find(ramaddr);
    if (ramit != ram.end()) addrname = ramit->second;
    auto ramvalit = ramvalues.find(ramaddr);
    if (ramvalit != ramvalues.end()) {
        auto valit = ramvalit->second.find(val);
        if (valit != ramvalit->second.end()) valname = valit->second;
    }

    // TODO: print amount in decimal?

    if (addrname && valname) {
        printf("%s[" ADDRFMT "] (%02x) WRITE %s = %s%s\n",
                spaces, instroff, instr, addrname, valname, hex);
    } else if (addrname) {
        printf("%s[" ADDRFMT "] (%02x) WRITE %s = 0x%04x%s\n",
                spaces, instroff, instr, addrname, (unsigned)val, hex);
    } else {
        printf("%s[" ADDRFMT "] (%02x) WRITE $%04x = 0x%04x%s\n",
                spaces, instroff, instr, (unsigned)ramaddr, (unsigned)val, hex);
    }
}
static void printwrite(const char* spaces, unsigned instroff, uint8_t instr, uint16_t ramaddr, const char* val, const char* hex="") // this is loRAM only
{
    const char* addrname = nullptr;
    auto ramit = ram.find(ramaddr);
    if (ramit != ram.end()) addrname = ramit->second;

    // TODO: print amount in decimal?

    if (addrname)
        printf("%s[" ADDRFMT "] (%02x) WRITE %s = %s%s\n",
                spaces, instroff, instr, addrname, val, hex);
    else
        printf("%s[" ADDRFMT "] (%02x) WRITE $%04x = %s%s\n",
                spaces, instroff, instr, ramaddr, val, hex);
}
#ifdef PRINT_HEX
static const char* buf_hexdump(const uint8_t* buf, uint32_t len, char* out, size_t outlen, uint32_t start, uint32_t end, bool ellipsis=false)
{
    out[0]=0;
    if (end-start==0 && !ellipsis)
        snprintf(out, outlen, "%s%02x%s", HEX_START, read8(start), HEX_END);
    else if (end-start==0) // && ellipsis
        snprintf(out, outlen, "%s%02x ...%s", HEX_START, read8(start), HEX_END);
    else if (end>start) {
        size_t endlen = strlen(HEX_END);
        uint32_t addr = start;
        int wr = snprintf(out, outlen, "%s%02x", HEX_START, read8(addr));
        addr++;
        while (outlen - wr > endlen+10/*6*/ && addr<=end) { // not perfect, but out should be big enough anyway
            wr += snprintf(out+wr, outlen-wr, " %02x", read8(addr));
            addr++;
        }
        if ((addr < end) || ellipsis)
            snprintf(out+wr, outlen-wr, " ...%s", HEX_END);
        else
            snprintf(out+wr, outlen-wr, "%s", HEX_END);
    }
    return out;
}
#else
#define buf_hexdump(...) ""
#endif
#define hexdump(out, outlen, start, end, ellipsis) buf_hexdump(buf, len, out, outlen, start, end, ellipsis)
#define _HD(start, end, ellipsis) hexdump(hexbuf, sizeof(hexbuf), start, end, ellipsis)
#define HDL(len) _HD(scriptstart+instroff, scriptaddr+len-1, false)
#define HD() HDL(0)
#define HDEL(len) _HD(scriptstart+instroff, scriptaddr+len-1, true)
#define HDE() HDEL(0)

// instruction opcode enum constants
enum {
    SCRIPT_END = 0x00,
    BRANCH = 0x04,              // unconditional branch
    BRANCH_NEG = 0x05,          // unconditional negative branch
    CALL_24BIT = 0x07,
    BRANCH_IF = 0x08,
    BRANCH_IF_NOT = 0x09,
    BRANCH_IF_MONEY_GE = 0xa,   // RJMP if moniez>=amount according to darkmoon
    BRANCH_IF_MONEY_LT = 0xb,   // if moniez<amount
    CALL_24BIT_N = 0x29,
    CALL_SUB = 0xa3,            // call sub script
    CALL_16BIT = 0xa4,          // some call instr with 16bit addr
    CALL_8BIT_NEG = 0xa5,       // negative call with 8bit offset
    CALL_16BIT_REL = 0xa6,      // looks like relative call with 16bit offset
    SLEEP_8BIT = 0xa7,          // sleep/delay frames
    SLEEP_16BIT = 0xa8,         // sleep/delay frames
};

static uint32_t min_addr = 0xffffffff;
static uint32_t max_addr = 0x00000000;
static unsigned instr_count = 0;
static uint16_t max_text = 0;
static unsigned unknowninstrs = 0;

static std::list<LootData> sniffs;
static std::list<LootData> gourds;
static std::map<uint32_t, DoggoData > change_doggo;

#define ADDR (absAddr ? (scriptstart+instroff) : instroff)
#define DST (unsigned)(absAddr ? (scriptstart+dst) : dst)
static void printscript(const char* spaces, const uint8_t* buf, uint32_t scriptaddr, size_t len, bool btrigger=false, uint8_t mapid=0, uint16_t scriptid=0, uint8_t x1=0, uint8_t y1=0, uint8_t x2=0, uint8_t y2=0, size_t depth=0)
{
    if (depth>3) return; // avoid recursion
    
    LootData loot(mapid,scriptid,x1,y1,x2,y2);

#ifdef PRINT_HEX
    char hexbuf[256];
#endif

    uint32_t scriptstart = scriptaddr;
    signed maxoff = 0; // used to detect if there is stuff after END
    std::list<signed> manualoffs; // this is used for jump targets inside code
    #define offs manualoffs
    //if (scriptaddr == ...) { // add manual offsets (due to undecoded instrs) like this
    //    offs.push_back(...);
    //} else if (scriptaddr == ...) {
    //    offs.push_back(...);
    //}
    #undef offs
    std::list<signed> offs = manualoffs; // this is used for jump targets inside code 
    
    if (!offs.empty()) maxoff = *std::max_element(offs.begin(), offs.end());

    bool last_decode_success = true;
    bool done = false;
    
    #define DONE() { done=true; break; }
    
    #define NEXT_INSTR() { \
        unknowninstrs++; \
        last_decode_success = false; \
        if (maxoff<=(signed)instroff) \
            DONE(); \
        offs.sort(); \
        for (auto off: offs) { \
            if (off>(signed)instroff) { \
                scriptaddr = scriptstart+off; \
                break; \
            } \
        } \
    }
    
    // NOTE: we would need a list of jump targets to always get all code paths
    while (!done) {
        instr_count++;
        
#ifndef NO_OFFSET_DEBUGGING // manual offsets should not be required as soon as the parser is complete
        if (last_decode_success && std::find(manualoffs.begin(), manualoffs.end(), (scriptaddr-scriptstart))!=manualoffs.end()) {
            printf("** USELESS MANUAL OFFSET 0x%06x for 0x%06x **\n",
                scriptaddr, scriptstart);
        }
        // TODO: also detect *wrong* manual offsets
#endif
        
        last_decode_success=true;
        
        if (scriptaddr<min_addr) min_addr = scriptaddr;
        if (scriptaddr>max_addr) max_addr = scriptaddr;
        unsigned instroff = scriptaddr-scriptstart;
        uint8_t instr = read8(scriptaddr++);
        uint8_t type = 0; // used for some instructions
        unsigned addr = 0; // used for some instructions
        uint16_t val16 = 0; // used for some instructions
        uint8_t N=0; // 8bit counter
        switch (instr) {
            case SCRIPT_END:
            {
                printf("%s[" ADDRFMT "] (%02d) END (return)%s\n", spaces, ADDR, instr, HD());
                unknowninstrs--;
                NEXT_INSTR();
                break;
            }
            case BRANCH: // 0x04, unconditional branch
            {
                int16_t jmp = (int16_t)read16(scriptaddr); scriptaddr+=2;
                signed dst = (signed)scriptaddr-scriptstart+jmp;
                printf("%s[" ADDRFMT "] (%02x) SKIP %d (to +x%02x)%s\n",
                    spaces, ADDR, instr, jmp, DST, HD());
                offs.push_back(dst);
                if (dst>maxoff) maxoff = dst;
                break;
            }
            case BRANCH_NEG: // 0x05, unconditional negative branch
            {
                int16_t jmp = (int16_t)read8(scriptaddr++)-0x100;
                signed dst = (signed)scriptaddr-2-scriptstart+jmp;
                printf("%s[" ADDRFMT "] (%02x) SKIP %d (to +x%02x)%s\n",
                    spaces, ADDR, instr, jmp, DST, HD());
                if (dst>=0) {
                    offs.push_back(dst);
                    if (dst>maxoff) maxoff = dst;
                } else {
                    // TODO: handle negative script offsets (I think this is not used in Evermore though)
                    fprintf(stderr, "WARN: (%02x) branch outside of script not implemented!\n", instr);
                }
                break;
            }
            case CALL_24BIT: // 0x07, CALL 24bit, what's the difference to instr 29 ?
            {
                addr = script2romaddr(read24(scriptaddr)); scriptaddr+=3;
#ifdef AUTO_DISCOVER_SCRIPTS
                // TODO: generate some unique name
                bool exists = false;
                for (const auto& s:absscripts) if (s.first == addr) { exists=true; break; }
                if (!exists) {
                    strings.push_back(strdup(("Unnamed ABS script " + u24val2str(addr)).c_str()));
                    absscripts.push_back(std::make_pair(addr, strings.back()));
                }
#endif
                printf("%s[" ADDRFMT "] (%02x) CALL 0x%06x %s%s\n",
                    spaces, ADDR, instr, addr, absscript2name(addr), HD());
                break;
            }
            case BRANCH_IF:     // 0x08
                bool condition;
                condition = true;
                // fall through
            case BRANCH_IF_NOT: // 0x09
                condition = false;
                // NOTE: this uses sub-instruction chaining, msbit in sub-instr set means last sub-instr
                //       we need to have special cases for sniff, gourds and doggo lists at the moment
                // TODO: reduce special cases to a minimum (unified parse_subinstr() is at the end)
                type = read8(scriptaddr++);
                if (type==0xd7) {
                    int16_t jmp = (int16_t)read16(scriptaddr); scriptaddr+=2;
                    signed dst = (signed)scriptaddr-scriptstart+jmp;
                    printf("%s[" ADDRFMT "] (%02x) IF controlled char %s dog SKIP %d (to +x%02x)%s\n",
                            spaces, ADDR, instr, condition ? "==" : "!=", jmp, DST, HD());
                    offs.push_back(dst);
                    if (dst>maxoff) maxoff = dst;
                } else if ((type&0xf0)==0x30 && read8(scriptaddr+0)==0x14 && read8(scriptaddr+1)==0x14 && read8(scriptaddr+2)==0x94) {
                    // This is a special case we use in the randomizer
                    scriptaddr+=3;
                    int16_t jmp = (int16_t)read16(scriptaddr); scriptaddr+=2;
                    signed dst = (signed)scriptaddr-scriptstart+jmp;
                    printf("%s[" ADDRFMT "] (%02x) IF !!!FALSE == %s SKIP %d (to +x%02x)%s\n",
                            spaces, ADDR, instr, (!condition)^(type!=0x30) ? "FALSE (never)" : "TRUE (always)", jmp, DST, HD());
                    offs.push_back(dst);
                    if (dst>maxoff) maxoff = dst;
                } else if ((type&0xf0)==0x30 && read8(scriptaddr+0)==0x14 && read8(scriptaddr+1)==0x94) {
                    // This is a special case we use in the randomizer
                    scriptaddr+=2;
                    int16_t jmp = (int16_t)read16(scriptaddr); scriptaddr+=2;
                    signed dst = (signed)scriptaddr-scriptstart+jmp;
                    printf("%s[" ADDRFMT "] (%02x) IF !!FALSE == %s SKIP %d (to +x%02x)%s\n",
                            spaces, ADDR, instr, (!condition)^(type!=0x30) ? "FALSE (always)" : "TRUE (never)", jmp, DST, HD());
                    offs.push_back(dst);
                    if (dst>maxoff) maxoff = dst;
                } else if (!condition && type==0x05 && read8(scriptaddr+2)==0x14 && read8(scriptaddr+3)==0x29 && read8(scriptaddr+4)==0x05 && read8(scriptaddr+7)==0x14 && read8(scriptaddr+8)==0xa8) {
                    // this is more readable than unified sub parser's output
                    // TODO: readable names as above
                    // traxx: 09 05 <addr,bp> 14 29 05 <addr,bp> 14 a8: IF (!test2 && !test2 == false) skip
                    val16 = read16(scriptaddr+0);
                    addr = 0x2258 + (val16>>3);
                    unsigned testbm = 1<<(val16&0x7);
                    val16 = read16(scriptaddr+5);
                    unsigned addr2 = 0x2258 + (val16>>3);
                    unsigned testbm2 = 1<<(val16&0x7);
                    int16_t jmp = (int16_t)read16(scriptaddr+9);
                    scriptaddr += 11;
                    signed dst = (signed)scriptaddr-scriptstart+jmp;
                    //printf("%s[" ADDRFMT "] IF ((! $%04x&0x%02x) AND (! $%04x&0x%02x)) == FALSE SKIP %d (to +x%02x)%s\n",
                    printf("%s[" ADDRFMT "] (%02x) IF (($%04x&0x%02x) OR ($%04x&0x%02x)) SKIP %d (to +x%02x)%s\n",
                            spaces, ADDR, instr, addr, testbm, addr2, testbm2, jmp, DST, HD());
                    offs.push_back(dst);
                    if (dst>maxoff) maxoff = dst;
                } else if (type == 0x85 || (type==0x05 && read8(scriptaddr+2)==0x94)) {
                    // this is supposed to be better readable than unified parser's output
                    // test bit, 0x09:0->jump, 0x08:1->jump
                    // TODO: also allow type==0x0a (temp bit)
                    bool invert = (!condition) ^ (type==0x05);
                    val16 = read16(scriptaddr); scriptaddr+=2;
                    if (type == 0x05) scriptaddr++; // skip over inversion
                    int16_t jmp = (int16_t)read16(scriptaddr); scriptaddr+=2;
                    signed dst = (signed)scriptaddr-scriptstart+jmp;
                    addr = 0x2258 + (val16>>3);
                    unsigned testbm = 1<<(val16&0x7);
                    std::string readable = rambit2str(addr, val16&0x7);
                    printf("%s[" ADDRFMT "] (%02x) IF %s$%04x&0x%02x%s %s%sSKIP %d (to +x%02x)%s\n",
                        spaces, ADDR, instr, invert?"!(":"", addr, testbm, invert?")":"", (invert && !readable.empty())?"NOT":"", readable.c_str(), jmp, DST, HD());
                    loot.check_flag = {addr,val16&0x7};
                    loot.dataset |= LootData::DataSet::check;
                    offs.push_back(dst);
                    if (dst>maxoff) maxoff = dst;
                } else if (type == 0x05 && read8(scriptaddr+2)==0x29 && read8(scriptaddr+3)==0x08 && read8(scriptaddr+6)==0x29
                             && read8(scriptaddr+7)==0x32 && read8(scriptaddr+8)==0x22 && read8(scriptaddr+9)==0xa8) {
                    // one stupid sniff spot...
                    val16 = read16(scriptaddr); scriptaddr+=2;
                    scriptaddr++; scriptaddr++; // 0x29 0x08
                    unsigned addr2 = 0x2258 + read16(scriptaddr); scriptaddr+=2;
                    scriptaddr++; // 0x29
                    unsigned val2 = read8(scriptaddr++) & 0x0f;
                    scriptaddr++; scriptaddr++; // 0x22 0xa8
                    int16_t jmp = (int16_t)read16(scriptaddr); scriptaddr+=2;
                    signed dst = (signed)scriptaddr-scriptstart+jmp;
                    addr = 0x2258 + (val16>>3);
                    unsigned testbm = 1<<(val16&0x7);
                    std::string readable = rambit2str(addr, val16&0x7);
                    printf("%s[" ADDRFMT "] (%02x) IF %s$%04x & 0x%02x %s%s %s$%04x==0x%02x SKIP %d (to +x%02x)%s\n",
                        spaces, ADDR, instr, (type==0x09?"!":""), addr, testbm, readable.c_str(), (type==0x09?"||":"&&"), (type==0x09?"!":""), addr2, val2, jmp, DST, HD());
                    loot.check_flag = {addr,val16&0x7};
                    loot.dataset |= LootData::DataSet::check;
                    offs.push_back(dst);
                    if (dst>maxoff) maxoff = dst;
                } else if (!condition && type == 0x88) {
                    // read value, 0->jump
                    addr = 0x2258 + read16(scriptaddr); scriptaddr+=2;
                    int16_t jmp = (int16_t)read16(scriptaddr); scriptaddr+=2;
                    signed dst = (signed)scriptaddr-scriptstart+jmp;
                    printf("%s[" ADDRFMT "] (%02x) IF $%04x == 0x00 SKIP %d (to +x%02x)%s\n",
                        spaces, ADDR, instr, addr, jmp, DST, HD());
                    offs.push_back(dst);
                    if (dst>maxoff) maxoff = dst;
                } else {
                    scriptaddr--;
                    bool ok = true;
                    int exprlen = 0;
                    std::string expr = parse_sub(scriptaddr,&ok,&exprlen);
                    if (ok) {
                        int16_t jmp = (int16_t)read16(scriptaddr); scriptaddr+=2;
                        signed dst = (signed)scriptaddr-scriptstart+jmp;
                        printf("%s[" ADDRFMT "] (%02x) IF %s%s%s%s THEN SKIP %d (to +x%02x)%s\n",
                               spaces, ADDR, instr, (condition||exprlen<2)?"": "(", expr.c_str(), (condition||exprlen<2)?"": ")", condition?"":" == FALSE", jmp, DST, HD());
                        offs.push_back(dst);
                        if (dst>maxoff) maxoff = dst;
                    } else {
                        printf("%s[" ADDRFMT "] " RED "(%02x) IF %s%s%s%s THEN SKIP ...?" NORMAL "%s\n",
                               spaces, ADDR, instr, (condition||exprlen<2)?"": "(", expr.c_str(), (condition||exprlen<2)?"": ")", condition?"":" == FALSE", HDE());
                        NEXT_INSTR();
                    }
                }
                break;
            case BRANCH_IF_MONEY_GE: // RJMP if moniez>=amount according to darkmoon
            case BRANCH_IF_MONEY_LT: // if moniez<amount
            {
                const char* op = (instr==0x0a) ? GE : LT;
                bool ok = true;
                std::string currency = sub2currency(parse_sub(scriptaddr,&ok));
                uint32_t v = read24(scriptaddr); scriptaddr+=3;
                int16_t jmp = (int16_t)read16(scriptaddr); scriptaddr+=2;
                signed dst = (signed)scriptaddr-scriptstart+jmp;
                if (ok) {
                    printf("%s[" ADDRFMT "] (%02x) IF %s (moniez) %s %u THEN SKIP %d (to +x%02x)%s\n",
                        spaces, ADDR, instr, currency.c_str(), op, v, jmp, DST, HD());
                    offs.push_back(dst);
                    if (dst>maxoff) maxoff = dst;
                } else {
                    printf("%s[" ADDRFMT "] " RED "(%02x) IF %s (moniez) %s ...? THEN SKIP ...?" NORMAL "%s\n",
                        spaces, ADDR, instr, currency.c_str(), op, HDE());
                    NEXT_INSTR();
                }
                break;
            }
            case 0x0c: // set or clear bit in 2258+x
            case 0x0d: // set or clear bit in 2834+x
            {
                // we still have some special cases for loot detection and readability
                val16 = read16(scriptaddr); scriptaddr+=2;
                addr = ((instr==0x0c)?0x2258:0x2834) + (val16>>3);
                type = read8(scriptaddr++);
                std::string readable = rambit2str(addr, val16&0x7);
                if (type == 0x85 || type == 0x8a || (type == 0x05 && read8(scriptaddr+2)==0x94) || (type == 0x0a && read8(scriptaddr+2)==0x94)) {
                    unsigned addr2 = (((type&0x7f)==0x05) ? 0x2258 : 0x2834) + (read16(scriptaddr)>>3);
                    unsigned bm2 = 1<<(read16(scriptaddr)&0x07);
                    scriptaddr+=2;
                    bool invert = ((type&0x80) == 0);
                    if (invert) scriptaddr++; // skip the 0x94
                    unsigned setbm = 1<<(val16&0x7);
                    printf("%s[" ADDRFMT "] (%02x) $%04x |= 0x%02x if (%s$%04x & 0x%02x) else $%04x &= ~0x%02x %s%s\n",
                        spaces, ADDR, instr, addr, setbm, invert?"!":"", addr2, bm2, addr, setbm, readable.c_str(), HD());
                    if (!invert && addr2==0x22ea) {
                        loot.set_flag = {addr,val16&0x7};
                        loot.dataset |= LootData::DataSet::set;
                    }
                } else if (type == 0xb0) {
                    unsigned setbm = 0xff & ~(1<<(val16&0x7));
                    printf("%s[" ADDRFMT "] (%02x) $%04x &= 0x%02x (8bit mode) %s%s\n",
                        spaces, ADDR, instr, addr, setbm, readable.c_str(), HD());
                } else if (sub1bIsFinalVal(type)/*type == 0xb1*/) {
                    unsigned setbm = 1<<(val16&0x7);
                    printf("%s[" ADDRFMT "] (%02x) $%04x |= 0x%02x %s%s\n",
                        spaces, ADDR, instr, addr, setbm, readable.c_str(), HD());
                } else {
                    scriptaddr--;
                    bool ok = true;
                    std::string v = parse_sub(scriptaddr,&ok);
                    if (ok) {
                        printf("%s[" ADDRFMT "] (%02x) $%04x bit 0x%02x = %s%s%s\n",
                            spaces, ADDR, instr, addr, 1<<(val16&7), v.c_str(), readable.c_str(), HD());
                    } else {
                        printf("%s[" ADDRFMT "] " RED "(%02x) $%04x bit 0x%02x = %s%s" NORMAL "%s\n",
                            spaces, ADDR, instr, addr, 1<<(val16&7), v.c_str(), readable.c_str(), HDE());
                        NEXT_INSTR();
                    }
                }
                break;
            }
            case 0x0e: // like 0c and 0d but for script args, not memory, and adress is only 8bit
            {
                val16 = read8(scriptaddr++);
                addr = val16>>3;
                bool ok = true;
                std::string v = parse_sub(scriptaddr,&ok);
                if (ok) {
                    printf("%s[" ADDRFMT "] (%02x) Script arg%d bit 0x%02x = %s%s\n",
                        spaces, ADDR, instr, addr, 1<<(val16&7), v.c_str(), HD());
                } else {
                    printf("%s[" ADDRFMT "] " RED "(%02x) Script arg%d bit 0x%02x = %s" NORMAL "%s\n",
                        spaces, ADDR, instr, addr, 1<<(val16&7), v.c_str(), HDE());
                    NEXT_INSTR();
                }
                break;
            }
            case 0x17: // set value fast
                addr  = 0x2258 + (unsigned)read16(scriptaddr); scriptaddr+=2;
                val16 = (unsigned)read16(scriptaddr); scriptaddr+=2;
                printwrite(spaces, ADDR, instr, addr, val16, HD());
                if (addr == 0x2391) {  // item
                    loot.item = val16;
                    loot.item_pos = scriptstart+instroff+3; // instr,addrhi:lo,val
                    loot.item_type = IntType::word;
                    loot.dataset |= LootData::DataSet::item;
                } else if (addr == 0x2393) { // amount
                    loot.amount = val16;
                    loot.amount_pos = scriptstart+instroff+3;
                    loot.amount_type = IntType::word;
                    loot.dataset |= LootData::DataSet::amount;
                } else if (addr == 0x2461) { // extra amount for next pickup
                    loot.extra = val16;
                    loot.extra_pos = scriptstart+instroff+3;
                    loot.extra_type = IntType::word;
                    loot.dataset |= LootData::DataSet::extra;
                } else if (addr == 0x2395) { // map ref
                    loot.mapref = val16;
                } else if (addr == 0x2443) { // change doggo
                    change_doggo[scriptstart+instroff+3] = { IntType::word, val16, mapid };
                }
                break;
            case 0x1a: // unknown, get current script addr, read 1 byte offset, run sub-instr, modify script
            case 0x1e:
            // sets script arg according to darkmoon. no clue what the difference between 1e and 1a is though
            {
                addr = read8(scriptaddr++);
                bool ok = true;
                std::string v = parse_sub(scriptaddr, &ok);
                if (ok) {
                    printf("%s[" ADDRFMT "] " UNTRACED "(%02x) WRITE SCRIPT arg%d = %s" NORMAL "%s\n",
                        spaces, ADDR, instr, addr, v.c_str(), HD());
                } else {
                    printf("%s[" ADDRFMT "] " RED "(%02x) WRITE SCRIPT arg%d = %s" NORMAL "%s\n",
                        spaces, ADDR, instr, addr, v.c_str(), HDE());
                    NEXT_INSTR();
                }
                break;
            }
            case 0x10: // same as 0x18, but only writes 1 byte, not two
            case 0x14: // same as 0x10
            case 0x11: // like 0x10 but different addr offset
            case 0x15: // same as 0x11
            case 0x18: // set word. what's the difference to 1C?
            case 0x19: // same as 0x18 but different addr. offset
            case 0x1c: // what's the difference to 0x18? $04 maybe?
            case 0x1d: // counter-part to 0x1c? what's the difference to 0x19?
            {
                // NOTE: we need a few special cases for loot data generation
                addr = ((instr==0x11||instr==0x15||instr==0x19||instr==0x1d)?0x2834:0x2258) + (unsigned)read16(scriptaddr); scriptaddr+=2;
                type = read8(scriptaddr++);
                uint8_t addrmode = 0;

                if (sub1bIsFinalVal(type)) {
                    addrmode = 3;
                    val16 = sub1b2val(type);
                    printwrite(spaces, ADDR, instr, addr, val16, HD());
                } else if (type == 0x82) { // write from byte
                    addrmode = 1;
                    val16 = read8(scriptaddr++);
                    printwrite(spaces, ADDR, instr, addr, val16, HD());
                } else if (type == 0x84) { // write from word
                    addrmode = 2;
                    val16 = read16(scriptaddr); scriptaddr+=2;
                    printwrite(spaces, ADDR, instr, addr, val16, HD());
                } else {
                    bool ok = true;
                    scriptaddr--;
                    std::string v = parse_sub(scriptaddr, &ok);
                    if (ok) {
                        printwrite(spaces, ADDR, instr, addr, v.c_str(), HD());
                    } else {
                        printf("%s[" ADDRFMT "] " RED "(%02x) WRITE $%04x = %s" NORMAL "%s\n",
                            spaces, ADDR, instr, addr, v.c_str(), HDE());
                        NEXT_INSTR();
                    }
                }
                
                unsigned _pos = (addrmode==1) ? scriptstart+instroff+4 : // byte
                                (addrmode==2) ? scriptstart+instroff+4 : // word
                                (addrmode==3) ? scriptstart+instroff+3 : // 1B sub-instr
                                (addrmode==4) ? scriptstart+instroff+3 : // 2B sub-instr
                                (addrmode==5) ? scriptstart+instroff+3 : // 3B sub-instr
                                                scriptstart+instroff; // for error reporting
                IntType _inttype = 
                                (addrmode==1) ? IntType::byte :
                                (addrmode==2) ? IntType::word :
                                (addrmode==3) ? IntType::subinstr1B :
                                (addrmode==4) ? IntType::subinstr2B :
                                (addrmode==5) ? IntType::subinstr3B:
                                                IntType::none;
                if (addr == 0x2391) {  // item
                    loot.item = val16;
                    loot.item_pos = _pos;
                    loot.item_type = _inttype;
                    loot.dataset |= LootData::DataSet::item;
                } else if (addr == 0x2393) { // amount
                    loot.amount = val16;
                    loot.amount_pos = _pos;
                    loot.amount_type = _inttype;
                    loot.dataset |= LootData::DataSet::amount;
                } else if (addr == 0x2461) { // extra amount for next pickup
                    loot.extra = val16;
                    loot.extra_pos = _pos;
                    loot.extra_type = _inttype;
                    loot.dataset |= LootData::DataSet::extra;
                } else if (addr == 0x2395) { // map ref
                    loot.mapref = val16;
                } else if (addr == 0x2443) { // change doggo
                    change_doggo[_pos] = { _inttype, val16, mapid };
                }
                
                break;
            }
            case 0x1b: // set two values fast
                {
                    unsigned addr1 = 0x2258 + read16(scriptaddr); scriptaddr+=2;
                    unsigned addr2 = 0x2258 + read16(scriptaddr); scriptaddr+=2;
                    unsigned val1 = ((unsigned)read8(scriptaddr++))<<3;
                    unsigned val2 = ((unsigned)read8(scriptaddr++))<<3;
                    printwrite(spaces, ADDR, instr, addr1, val1);
                    printwrite(spaces, ADDR, instr, addr2, val2, HD());
                }
                break;
            case 0x20: // Teleport both characters (Unknown, writes to boy data and does more stuff)
                {
                    uint8_t arg1 = read8(scriptaddr++);
                    uint8_t arg2 = read8(scriptaddr++);
                    printf("%s[" ADDRFMT "] (%02x) Teleport both to %02x %02x%s\n",
                        spaces, ADDR, instr, arg1, arg2, HD());
                }
                break;
            case 0x22: // change map
                {
                    uint16_t x = (unsigned)read8(scriptaddr++) << 3;
                    uint16_t y = (unsigned)read8(scriptaddr++) << 3;
                    uint8_t  id = read16(scriptaddr); scriptaddr+=2; // 16bit for some reason
                    const char* mapname = "";
                    for (auto pair: maps) {
                        if (pair.first == id) {
                            mapname = pair.second;
                            break;
                        }
                    }
                    printf("%s[" ADDRFMT "] (%02x) CHANGE MAP = 0x%02x @ [ 0x%04x | 0x%04x ]%s%s%s%s\n",
                        spaces, ADDR, instr, 
                        (unsigned)id, (unsigned)x, (unsigned)y,
                        *mapname?": \"":"", mapname, *mapname?"\"":"", HD());
                }
                break;
            case 0x26:
                printf("%s[" ADDRFMT "] " UNTRACED "(%02x) UNTRACED INSTR, writing to VRAM" NORMAL "%s\n",
                    spaces, ADDR, instr, HD());
                break;
            case 0x27: // Unknown, Writes 0x8000 -> $0b83.
                printf("%s[" ADDRFMT "] (%02x) Fade-out screen (WRITE $0b83=0x8000)%s\n",
                    spaces, ADDR, instr, HD());
                break;
            case CALL_24BIT_N: // 0x29, CALL 24bit, what's the difference to instr 0x07?
            {
                addr = script2romaddr(read24(scriptaddr)); scriptaddr+=3;
#ifdef AUTO_DISCOVER_SCRIPTS
                // TODO: generate some unique name
                bool exists = false;
                for (const auto& s:absscripts) if (s.first == addr) { exists=true; break; }
                if (!exists) {
                    strings.push_back(strdup(("Unnamed ABS script " + u24val2str(addr)).c_str()));
                    absscripts.push_back(std::make_pair(addr, strings.back()));
                }
#endif
                printf("%s[" ADDRFMT "] (%02x) CALL 0x%06x %s%s\n",
                    spaces, ADDR, instr, addr, absscript2name(addr), HD());
                break;
            }
            case 0x2c:
            case 0x2d: // oppisite of 2c?
                type = read8(scriptaddr++);
                if (type==0x08) {
                    printf("%s[" ADDRFMT "] " UNTRACED "(%02x) UNTRACED INSTR for script caller (0x%02x)" NORMAL "%s\n",
                        spaces, ADDR, instr, type, HD());
                } else {
                    printf("%s[" ADDRFMT "] " RED "(%02x) UNKNOWN INSTR, arg 0x%02x" NORMAL "%s\n",
                        spaces, ADDR, instr, type, HDE());
                }
                break;
            case 0x2a: // disable/freeze character from sub-instr
            case 0x2b: // enables/unfreeze character from sub-instr
            {
                const char* op = (instr==0x2a) ? "script controlled" : "player/AI controlled";
                bool ok = true;
                std::string entity = parse_sub(scriptaddr,&ok);
                if (ok) {
                    printf("%s[" ADDRFMT "] (%02x) Make %s %s%s\n",
                        spaces, ADDR, instr, entity.c_str(), op, HD());
                } else {
                    printf("%s[" ADDRFMT "] " RED "(%02x) Make %s %s" NORMAL "%s\n",
                        spaces, ADDR, instr, entity.c_str(), op, HDE());
                    NEXT_INSTR();
                }
                break;
            }
            case 0x2e: // get entity pointer using sub-instr, modify some object in 3bc9+(pointer+0x6c), modify current script (timer?)
                type = read8(scriptaddr++);
                if (type==0x8d || type==0x88) {
                    addr = ((type==0x8d)?0x2834:0x2258)+read16(scriptaddr); scriptaddr+=2;
                    printf("%s[" ADDRFMT "] (%02x) Wait for entity from *$%04x to reach destination%s\n",
                        spaces, ADDR, instr, addr, HD());
                } else if (subinstrIsEntityOnly(type)) {
                    printf("%s[" ADDRFMT "] (%02x) Wait for %s (%02x) to reach destination%s\n",
                        spaces, ADDR, instr, subinstr2name(type), type, HD());
                } else if (sub1bIsFinalVal(type)) {
                    printf("%s[" ADDRFMT "] (%02x) Wait for character #%u ?! to reach destination%s\n",
                        spaces, ADDR, instr, sub1b2val(type), HD());
                } else if (type == 0x84) {
                    val16 = read16(scriptaddr); scriptaddr+=2;
                    printf("%s[" ADDRFMT "] (%02x) Wait for character #%u ?! to reach destination%s\n",
                        spaces, ADDR, instr, (type&0x0f), HD());
                } else if (type == 0x92) {
                    addr = read8(scriptaddr++);
                    printf("%s[" ADDRFMT "] (%02x) Wait for character from sub-instr %02x %02x to reach destination%s\n",
                        spaces, ADDR, instr, type, addr, HD());
                } else if (type==0x04 && read8(scriptaddr+2)==0x29 && (read8(scriptaddr+3)==0x08||read8(scriptaddr+3)==0x0d) && read8(scriptaddr+6)==0x9a) { // quick sand fields
                    uint16_t off = read16(scriptaddr);
                    addr = ((read8(scriptaddr+3)==0x0d)?0x2834:0x2258)+read16(scriptaddr+4);
                    scriptaddr+=7;
                    printf("%s[" ADDRFMT "] (%02x) Wait for entity from *$%04x +0x%04x to reach destination%s\n",
                        spaces, ADDR, instr, addr, off, HD());
                } else {
                    printf("%s[" ADDRFMT "] " RED "(%02x) Wait for unknown to reach destination 0x%02x ..." NORMAL "%s\n",
                        spaces, ADDR, instr, type, HDE());
                    NEXT_INSTR();
                }
                break;
            case 0x30: // unknown, accesses APU, writes to "current sound effect" variable
            case 0x31: // according to darkmoon this
            case 0x32: // and this is also sound effect. any difference?
                type = read8(scriptaddr++);
                printf("%s[" ADDRFMT "] (%02x) PLAY SOUND EFFECT 0x%02x ??%s\n",
                    spaces, ADDR, instr, type, HD());
                break;
            case 0x33: // unknown, accesses APU
                type = read8(scriptaddr++);
                printf("%s[" ADDRFMT "] (%02x) PLAY MUSIC 0x%02x%s\n",
                    spaces, ADDR, instr, type, HD());
                break;
            case 0x38: // <- according to darkmoon. i only traced 3a
            case 0x3a: // breaks out of script loop (to handle other stuff?)
                printf("%s[" ADDRFMT "] (%02x) YIELD (break out of script loop, continue later)%s\n",
                    spaces, ADDR, instr, HD());
                break;
            case 0x39: // sleep from subinstr according to darkmoon
            case 0x3b: // the same according to darkmoon
            {
                bool ok = true;
                std::string v = parse_sub(scriptaddr,&ok);
                if (ok) {
                    printf("%s[" ADDRFMT "] (%02x) SLEEP %s TICKS%s\n",
                        spaces, ADDR, instr, v.c_str(), HD());
                } else {
                    printf("%s[" ADDRFMT "] " RED "(%02x) SLEEP %s TICKS" NORMAL "%s\n",
                        spaces, ADDR, instr, v.c_str(), HD());
                    NEXT_INSTR();
                }
                break;
            }
            case 0x3c: // the >>1 below according to darkmoon (npc addr -> npc index)
            {
                    uint16_t arg1 = read16(scriptaddr); scriptaddr+=2;
                    uint16_t arg2 = read16(scriptaddr); scriptaddr+=2;
                    uint8_t  arg3 = read8(scriptaddr++);
                    uint8_t  arg4 = read8(scriptaddr++);
                    printf("%s[" ADDRFMT "] (%02x) Load NPC %04x" GT GT "1 flags/state %04x at pos %02x %02x%s\n",
                        spaces, ADDR, instr, arg1, arg2, arg3, arg4, HD());
                    break;
            }
            case 0x3d: // set NPC talk script
            {
                bool ok = true;
                std::string entity = parse_sub(scriptaddr,&ok);
                val16 = read16(scriptaddr); scriptaddr+=2;
                if (ok) {
#ifdef AUTO_DISCOVER_SCRIPTS
                    bool exists = false;
                    for (const auto& s:npcscripts) if (s.first == val16) { exists=true; break; }
                    if (!exists) {
                        strings.push_back(strdup(("Unnamed NPC talk script " + u16val2str(val16)).c_str()));
                        npcscripts.push_back(std::make_pair(val16, strings.back()));
                    }
#endif
                    printf("%s[" ADDRFMT "] (%02x) WRITE %s+x66=0x%02x, %s+x68=0x0040 (talk script): %s%s\n",
                        spaces, ADDR, instr, entity.c_str(), val16, entity.c_str(), npcscript2name(val16), HD());
                } else {
                    printf("%s[" ADDRFMT "] " RED "(%02x) WRITE %s+x66=0x%02x, %s+x68=0x0040 (talk script): %s" NORMAL "%s\n",
                        spaces, ADDR, instr, entity.c_str(), val16, entity.c_str(), npcscript2name(val16), HDE());
                    NEXT_INSTR();
                }
                break;
            }
            case 0x3f: // set NPC script
            {
                type = read8(scriptaddr++);
                if (sub1bIsFinalVal(type)) { // x=positive nibble, - 1, << 1
                    // looks like a different code path for non-pointers // TODO: RETRACE?
                    unsigned val1 = (unsigned)read16(scriptaddr); scriptaddr+=2;
                    unsigned val2 = (unsigned)read16(scriptaddr); scriptaddr+=2;
#ifdef AUTO_DISCOVER_SCRIPTS
                    // TODO: see what type actually means and name script accordingly
                    bool exists = false;
                    for (const auto& s:npcscripts) if (s.first == val16) { exists=true; break; }
                    if (!exists) {
                        strings.push_back(strdup(("Unnamed Short script " + u16val2str(val16)).c_str()));
                        npcscripts.push_back(std::make_pair(val16, strings.back()));
                    }
#endif
                    printf("%s[" ADDRFMT "] (%02x) WRITE $0ea2+%x=0x%02x, $0eac+%x=0x%02x (unknown): %s?%s\n",
                        spaces, ADDR, instr,
                        (sub1b2val(type)-1)<<1, val1, (sub1b2val(type)-1)<<1, val2,
                        npcscript2name(val2), HD());
                } else {
                    scriptaddr--;
                    bool ok = true;
                    std::string entity = parse_sub(scriptaddr,&ok);
                    if (ok) {
                        unsigned val1 = (unsigned)read16(scriptaddr); scriptaddr+=2;
                        unsigned val2 = (unsigned)read16(scriptaddr); scriptaddr+=2;
#ifdef AUTO_DISCOVER_SCRIPTS
                        bool exists = false;
                        for (const auto& s:npcscripts) if (s.first == val16) { exists=true; break; }
                        if (!exists) {
                            strings.push_back(strdup((
                                                  ( (val1==0x040)?"Unnamed NPC Talk script":
                                                    (val1==0x100)?"Unnamed NPC Damage script":
                                                    (val1==0x200)?"Unnamed NPC Kill script" : "Unnamed NPC script"
                                                  ) + u16val2str(val16)).c_str()));
                            npcscripts.push_back(std::make_pair(val16, strings.back()));
                        }
#endif
                        printf("%s[" ADDRFMT "] (%02x) WRITE %s+x68=0x%02x, %s+x66=0x%02x (set script): %s%s\n",
                            spaces, ADDR, instr, entity.c_str(), val1, entity.c_str(), val2, npcscript2name(val2), HD());
                    } else {
                        printf("%s[" ADDRFMT "] " RED "(%02x) UNKNOWN INSTR sub 0x%02x" NORMAL "%s\n",
                            spaces, ADDR, instr, type, HDE());
                        NEXT_INSTR();
                    }
                }
                break;
            }
            case 0x42: // Teleport entity from sub-instr to byte,byte
            {
                // TODO: verify X,Y is not swapped
                const char* toby = "to";
                bool ok = true;
                std::string entity = parse_sub(scriptaddr, &ok);
                val16 = read16(scriptaddr); scriptaddr+=2;
                if (ok && val16==0x0101) {
                    printf("%s[" ADDRFMT "] (%02x) Teleport %s %s 1,1 (hidden)%s\n",
                        spaces, ADDR, instr, entity.c_str(), toby, HD());
                } else if (ok) {
                    printf("%s[" ADDRFMT "] (%02x) Teleport %s %s %02x, %02x%s\n",
                        spaces, ADDR, instr, entity.c_str(), toby, val16>>8, val16&0xff, HD());
                } else {
                    printf("%s[" ADDRFMT "] " RED "(%02x) Teleport %s %s %02x, %02x" NORMAL "%s\n",
                        spaces, ADDR, instr, entity.c_str(), toby, val16>>8, val16&0xff, HDE());
                    NEXT_INSTR();
                }
                break;
            }
            case 0x43: // Teleport entity from sub-instr to sub-instr,sub-instr
                // fall through
            case 0xb9: // relative teleport according to darkmoon
            {
                // TODO: verify X,Y is not swapped
                const char* toby = (instr==0x43)?"to":"by";
                bool ok=true;
                std::string entity = parse_sub(scriptaddr, &ok);
                std::string x = ok?parse_sub(scriptaddr, &ok):"?";
                std::string y = ok?parse_sub(scriptaddr, &ok):"?";
                if (ok) {
                    printf("%s[" ADDRFMT "] (%02x) Teleport %s %s x:%s, y:%s%s\n",
                        spaces, ADDR, instr, entity.c_str(), toby, x.c_str(), y.c_str(), HD());
                } else {
                    printf("%s[" ADDRFMT "] " RED " (%02x) Teleport %s %s x:%s, y:%s" NORMAL "%s\n",
                        spaces, ADDR, instr, entity.c_str(), toby, x.c_str(), y.c_str(), HDE());
                    NEXT_INSTR();
                }
                break;
            }
            case 0x96: // relative teleport player screens and scroll according to darkmoon
            {
                // TODO: verify X,Y is not swapped
                const char* toby = (instr==0x96)?"by":"to";
                bool ok=true;
                std::string x = ok?parse_sub(scriptaddr, &ok):"?";
                std::string y = ok?parse_sub(scriptaddr, &ok):"?";
                if (ok) {
                    printf("%s[" ADDRFMT "] (%02x) Teleport player %s %s, %s screens%s\n",
                        spaces, ADDR, instr, toby, x.c_str(), y.c_str(), HD());
                } else {
                    printf("%s[" ADDRFMT "] " RED "(%02x) Teleport player %s %s, %s screens" NORMAL "%s\n",
                        spaces, ADDR, instr, toby, x.c_str(), y.c_str(), HDE());
                    NEXT_INSTR();
                }
                break;
            }
            case 0x44: // open messagebox and wait for next frame?
            case 0x45: // according to darkmoon, this,
            case 0x46: // this
            case 0x47: // and this are the same as 0x44
            {
                uint8_t msg_slot = read8(scriptaddr++);
                uint8_t msg_x = read8(scriptaddr++);
                uint8_t msg_y = read8(scriptaddr++);
                uint8_t msg_w = read8(scriptaddr++);
                uint8_t msg_h = read8(scriptaddr++);
                printf("%s[" ADDRFMT "] " UNTRACED "(%02x) UNTRACED INSTR, Open messagebox? slot=0x%02x x=0x%02x y=0x%02x w=0x%02x h=0x%02x" NORMAL "%s\n",
                    spaces, ADDR, instr, msg_slot, msg_x, msg_y, msg_w, msg_h, HD());
                break;
            }
            case 0x48: // according to darkmoon the same as 44 but default values instead of operands
            case 0x49:
            case 0x4A:
            case 0x4B:
            {
                printf("%s[" ADDRFMT "] " UNTRACED "(%02x) UNTRACED INSTR, Open default messagebox?" NORMAL "%s\n",
                    spaces, ADDR, instr, HD());
                break;
            }
            case 0x4d: // NOP according to darkmoon
                printf("%s[" ADDRFMT "] (%02x) NOP%s\n",
                    spaces, ADDR, instr, HD());
                break;
            case 0x4e:
            {
                bool ok = true;
                std::string entity = parse_sub(scriptaddr,&ok);
                if (ok) {
                    printf("%s[" ADDRFMT "] (%02x) ATTACH entity %s TO SCRIPT%s\n",
                        spaces, ADDR, instr, entity.c_str(), HD());
                } else {
                    printf("%s[" ADDRFMT "] " RED "(%02x) ATTACH entity %s TO SCRIPT" NORMAL "%s\n",
                        spaces, ADDR, instr, entity.c_str(), HDE());
                    NEXT_INSTR();
                }
                break;
            }
            case 0x50: // like 0x51, but 1 byte "destination in vram"
                type = read8(scriptaddr++);
                addr = 0x91d000 + read16(scriptaddr); scriptaddr+=2;
                printf("%s[" ADDRFMT "] (%02x) SHOW TEXT %04x FROM 0x%06x %s (UNWINDOWED) IN #%d%s\n"
                       "%s        %s\n",
                    spaces, ADDR, instr, read16(scriptaddr-2), addr,
                    read24(addr)&0x800000 ? "compressed" : "uncompressed", type, HD(),
                    spaces, get_text_i(addr,spaces,"                ").c_str());
                if (read16(scriptaddr-2) > max_text) max_text=read16(scriptaddr-2);
                break;
            case 0x51: // set text from word list in next two bytes + 91d000
                addr = 0x91d000 + read16(scriptaddr); scriptaddr+=2;
                printf("%s[" ADDRFMT "] (%02x) SHOW TEXT %04x FROM 0x%06x %s WINDOWED%s\n"
                       "%s        %s\n",
                    spaces, ADDR, instr, read16(scriptaddr-2), addr,
                    read24(addr)&0x800000 ? "compressed" : "uncompressed", HD(),
                    spaces, get_text_i(addr,spaces,"                ").c_str());
                if (read16(scriptaddr-2) > max_text) max_text=read16(scriptaddr-2);
                break;
            case 0x52: // set text from word list in next two bytes + 91d000
                addr = 0x91d000 + read16(scriptaddr); scriptaddr+=2;
                printf("%s[" ADDRFMT "] (%02x) SHOW TEXT %04x FROM 0x%06x %s UNWINDOWED%s\n"
                       "%s        %s\n",
                    spaces, ADDR, instr, read16(scriptaddr-2), addr,
                    read24(addr)&0x800000 ? "compressed" : "uncompressed", HD(),
                    spaces, get_text_i(addr,spaces,"                ").c_str());
                if (read16(scriptaddr-2) > max_text) max_text=read16(scriptaddr-2);
                break;
            case 0x54: // like 0x55, but 1 byte "destination in vram"
                type = read8(scriptaddr++);
                printf("%s[" ADDRFMT "] (%02x) CLEAR TEXT IN #%d%s\n",
                    spaces, ADDR, instr, type, HD());
                break;
            case 0x55:
                printf("%s[" ADDRFMT "] (%02x) CLEAR TEXT%s\n",
                    spaces, ADDR, instr, HD());
                break;
            case 0x58: // fade-in volume according to darkmoon
                printf("%s[" ADDRFMT "] (%02x) FADE IN VOLUME%s\n",
                    spaces, ADDR, instr, HD());
                break;
            case 0x59: // fade-out volume according to darkmoon
                printf("%s[" ADDRFMT "] (%02x) FADE OUT VOLUME%s\n",
                    spaces, ADDR, instr, HD());
                break;
            case 0x5a: // unknown, checks timer of unframed messages
                printf("%s[" ADDRFMT "] " UNTRACED "(%02x) UNTRACED INSTR, checking message timer" NORMAL "%s\n",
                    spaces, ADDR, instr, HD());
                break;
            case 0x5b: // unknown, checks timer of unframed messages
                printf("%s[" ADDRFMT "] " UNTRACED "(%02x) UNTRACED INSTR, checking message timer" NORMAL "%s\n",
                    spaces, ADDR, instr, HD());
                break;
            case 0x5c: // sets gourds' state (object from sub-instr, value from sub-instr)
            {
                bool ok = true;
                std::string obj = parse_sub(scriptaddr,&ok);
                std::string v = ok?parse_sub(scriptaddr,&ok):"?";
                // TODO: force hex for obj and v?
                if (ok) {
                    printf("%s[" ADDRFMT "] (%02x) SET OBJ %s STATE = val:%s (load/unload)%s\n",
                        spaces, ADDR, instr, obj.c_str(), v.c_str(), HD());
                } else {
                    printf("%s[" ADDRFMT "] " RED "(%02x) SET OBJ %s STATE = val:%s (load/unload)" NORMAL "%s\n",
                        spaces, ADDR, instr, obj.c_str(), v.c_str(), HDE());
                    NEXT_INSTR();
                }
                break;
            }
            case 0x5d: // conditional unload? 2bytes addr:bit, sub-instr for obj/sprite id
            {
                bool ok = true;
                std::string obj = parse_sub(scriptaddr,&ok);
                // TODO: force hex for obj?
                if (ok) {
                    val16 = read16(scriptaddr); scriptaddr+=2;
                    addr = 0x2258 + (val16>>3);
                    unsigned setbm = (1<<(val16&0x7));
                    printf("%s[" ADDRFMT "] (%02x) IF $%04x & 0x%02x THEN UNLOAD OBJ %s " RED "(TODO: verify this)" NORMAL "%s\n",
                        spaces, ADDR, instr, addr, setbm, obj.c_str(), HD());
                } else {
                    printf("%s[" ADDRFMT "] " RED "(%02x) IF ...? THEN UNLOAD OBJ %s" NORMAL "%s\n",
                        spaces, ADDR, instr, obj.c_str(), HDE());
                    NEXT_INSTR();
                }
                break;
            }
            case 0x62: // reads byte, reads two words, copies some data from unknown region to unknown region
            {
                printf("%s[" ADDRFMT "] " UNTRACED "(%02x) UNTRACED INSTR vals 0x%02x 0x%04x 0x%04x%s" NORMAL "\n",
                    spaces, ADDR, instr, read8(scriptaddr), read16(scriptaddr+1), read16(scriptaddr+3), HDL(5));
                scriptaddr += 5;
                break;
            }
            case 0x63:
                printf("%s[" ADDRFMT "] (%02x) SHOW ALCHEMY SELECTION SCREEN\n",
                    spaces, ADDR, instr);
                break;
            case 0x6c: // use sub-instr to get some entity, read 2B unknown
            {
                bool ok = true;
                std::string entity = parse_sub(scriptaddr, &ok);
                uint8_t a = read8(scriptaddr++);
                uint8_t b = read8(scriptaddr++);
                if (ok) {
                    printf("%s[" ADDRFMT "] " UNTRACED "(%02x) UNTRACED INSTR for %s with val1=0x%02x,val2=0x%02x" NORMAL "%s\n",
                        spaces, ADDR, instr, entity.c_str(), a, b, HD());
                } else {
                    printf("%s[" ADDRFMT "] " RED "(%02x) UNKNOWN INSTR for %s ..." NORMAL "%s\n",
                        spaces, ADDR, instr, entity.c_str(), HDE());
                    NEXT_INSTR();
                }
                break;
            }
            case 0x6e: // get entity pointer using sub-instr, create some object in 3bc9+*3bc7, attach to pointer+0x6c
            {
                bool ok = true;
                std::string entity = parse_sub(scriptaddr, &ok);
                if (ok) {
                    uint8_t x = read8(scriptaddr++); uint8_t y = read8(scriptaddr++);
                    printf("%s[" ADDRFMT "] (%02x) Make %s walk to x=0x%02x,y=0x%02x%s\n",
                        spaces, ADDR, instr, entity.c_str(), x, y, HD());
                } else {
                    printf("%s[" ADDRFMT "] " RED "(%02x) Make %s walk to ...?" NORMAL "%s\n",
                        spaces, ADDR, instr, entity.c_str(), HDE());
                    NEXT_INSTR();
                }
                break;
            }
            case 0x6d: // identical code path to 6f but stz $12 instead of #$1->$12. No clue where this is used
            case 0x6f: // get entity pointer using sub-instr, run 2 sub-instrs to get X and Y. Looks imilar to 6e
            case 0x73: // identical format but absolute position, not relative
            case 0x9d: // like 73 but does not ignore barriers
                // 6d honours barriers, 6f and 73 ignores barriers
            {
                bool ok = true;
                std::string entity = parse_sub(scriptaddr, &ok);
                std::string x = ok ? parse_sub(scriptaddr, &ok) : "?";
                std::string y = ok ? parse_sub(scriptaddr, &ok) : "?";
                const char* toby = (instr==0x73||instr==0x9d) ? "to" : "by";
                const char* direct = (instr==0x6f || instr==0x73) ? " directly" : ""; // ignoring barriers
                if (ok) {
                    printf("%s[" ADDRFMT "] (%02x) Make %s walk %s %s,%s%s%s\n",
                        spaces, ADDR, instr, entity.c_str(), toby, x.c_str(), y.c_str(), direct, HD());
                } else {
                    printf("%s[" ADDRFMT "] " RED "(%02x) Make %s walk %s %s,%s%s" NORMAL "%s\n",
                        spaces, ADDR, instr, entity.c_str(), toby, x.c_str(), y.c_str(), direct, HDE());
                    NEXT_INSTR();
                }
                break;
                }
            case 0x70: // make entity from sub-instr face entity from sub-instr
            case 0x71: // face each other
            {
                const char* s1 = instr==0x70 ? "face" : "and";
                const char* s2 = instr==0x70 ? "" : "face each other";
                bool ok = true;
                std::string entity1 = parse_sub(scriptaddr, &ok);
                std::string entity2 = ok ? parse_sub(scriptaddr, &ok) : "?";
                if (ok) {
                    printf("%s[" ADDRFMT "] (%02x) Make %s %s %s %s%s\n",
                        spaces, ADDR, instr, entity1.c_str(), s1, entity2.c_str(), s2, HD());
                } else {
                    printf("%s[" ADDRFMT "] " RED "(%02x) Make %s %s %s %s" NORMAL "%s\n",
                        spaces, ADDR, instr, entity1.c_str(), s1, entity2.c_str(), s2, HDE());
                    NEXT_INSTR();
                }
                break;
            }
            case 0x74:
            case 0x75:
            case 0x76:
            case 0x77:
            {
                const char* dir = instr==0x77 ? "EAST" : instr==0x75 ? "SOUTH" : instr==0x74 ? "NORTH" : "WEST";
                bool ok = true;
                std::string entity = parse_sub(scriptaddr, &ok);
                if (ok) {
                    printf("%s[" ADDRFMT "] (%02x) MAKE %s FACE %s%s\n",
                        spaces, ADDR, instr, entity.c_str(), dir, HD());
                } else {
                    printf("%s[" ADDRFMT "] " RED "(%02x) MAKE %s FACE %s" NORMAL "%s\n",
                        spaces, ADDR, instr, entity.c_str(), dir, HDE());
                    NEXT_INSTR();
                }
                break;
            }
            case 0x78: // unknown. changes NPC sprite/animation (maybe also other parameters)
            case 0x79: // similar to 78, used after washing ashore in Crustacia
                // see google docs for combinations we have so far
            {
                bool ok = true;
                std::string entity = parse_sub(scriptaddr, &ok);
                val16 = read16(scriptaddr); scriptaddr+=2;
                std::string v = ok?parse_sub(scriptaddr, &ok):"?";
                if (ok) {
                    printf("%s[" ADDRFMT "] " UNTRACED "(%02x) UNTRACED INSTR for %s, 0x%04x %s changes sprite/animation/...?" NORMAL "%s\n",
                        spaces, ADDR, instr, entity.c_str(), val16, v.c_str(), HD());
                } else {
                    printf("%s[" ADDRFMT "] " RED "(%02x) UNKNOWN INSTR for %s, 0x%04x %s changes sprite/animation/...?" NORMAL "%s\n",
                        spaces, ADDR, instr, entity.c_str(), val16, v.c_str(), HDE());
                    NEXT_INSTR();
                }
                break;
            }
            case 0x7a: // *(sub-instr) = (sub-instr) according to darkmoon
            {
                bool ok = true;
                std::string dst = parse_sub(scriptaddr,&ok);
                std::string src = ok?parse_sub(scriptaddr,&ok):"?";
                if (ok) {
                    printf("%s[" ADDRFMT "] (%02x) WRITE *(%s) = %s%s\n",
                           spaces, ADDR, instr, dst.c_str(), src.c_str(), HD());
                } else {
                    printf("%s[" ADDRFMT "] " RED "(%02x) WRITE *(%s) = %s" NORMAL "%s\n",
                           spaces, ADDR, instr, dst.c_str(), src.c_str(), HDE());
                    NEXT_INSTR();
                }
                break;
            }
            case 0x7c: // give moniez
            case 0x7d: // take moniez
            {
                const char* op = (instr==0x7c)?"Give":"Take";
                bool ok = true;
                std::string currency = sub2currency(parse_sub(scriptaddr, &ok));
                uint32_t moniez = read24(scriptaddr); scriptaddr+=3;
                if (ok) {
                    printf("%s[" ADDRFMT "] (%02x) %s %u %s (moniez)%s\n",
                        spaces, ADDR, instr, op, (unsigned)moniez, currency.c_str(), HD());
                } else {
                    printf("%s[" ADDRFMT "] " RED "(%02x) %s unknown %s (moniez)" NORMAL "%s\n",
                        spaces, ADDR, instr, op, currency.c_str(), HDE());
                    NEXT_INSTR();
                }
                break;
            }
            case 0x7e: // exchange moniez
            {
                bool ok = true;
                std::string mul = parse_sub(scriptaddr,&ok);
                std::string src = ok?sub2currency(parse_sub(scriptaddr,&ok)):"?";
                std::string div = ok?parse_sub(scriptaddr,&ok):"?";
                std::string dst = ok?sub2currency(parse_sub(scriptaddr,&ok)):"?";
                if (ok) {
                    printf("%s[" ADDRFMT "] (%02x) Exchange %s %s to %s %s (moniez)%s\n",
                        spaces, ADDR, instr, mul.c_str(), src.c_str(), div.c_str(), dst.c_str(), HD());
                } else {
                    printf("%s[" ADDRFMT "] " RED "(%02x) Exchange %s %s to %s %s (moniez)" NORMAL "%s\n",
                        spaces, ADDR, instr, mul.c_str(), src.c_str(), div.c_str(), dst.c_str(), HDE());
                    NEXT_INSTR();
                }
                break;
            }
            case 0x7f: // show text input (dog's name)
                val16 = read16(scriptaddr); scriptaddr+=2;
                printf("%s[" ADDRFMT "] (%02x) SHOW TEXT/NAME INPUT 0x%04x%s\n",
                       spaces, ADDR, instr, val16, HD());
                break;
            case 0x80: // unknown, clears msbit in unframed message timer
            case 0x81: // unknown, at 8ce209
                {
                const char* action = (instr==0x80) ? "UNHIDE?" : "HIDE";
                printf("%s[" ADDRFMT "] (%02x) %s UNWINDOWED TEXT%s\n",
                    spaces, ADDR, instr, action, HD());
                }
                break;
            case 0x82: // set colors according to darkmoon?! - need to retrace this at some point
                printf("%s[" ADDRFMT "] " UNTRACED "(%02x) Also change visible layers?" NORMAL "%s\n",
                    spaces, ADDR, instr, HD());
                break;
            case 0x83: // (updates what layers are rendered, called after variables are modified)
                printf("%s[" ADDRFMT "] (%02x) Change visible layers, ... based on $7e0f80..7e0f83%s\n",
                    spaces, ADDR, instr, HD());
                break;
            case 0x84: // give moniez, like 7c but amount is sub-instr
            case 0x85: // take moniez, like 7d but amount is sub-instr
            {
                const char* op = (instr==0x84)?"Give":"Take";
                bool ok=true;
                std::string currency = sub2currency(parse_sub(scriptaddr, &ok));
                std::string moniez = ok?parse_sub(scriptaddr,&ok):"?";
                if (ok) {
                    printf("%s[" ADDRFMT "] (%02x) %s %s %s (moniez)%s\n",
                        spaces, ADDR, instr, op, moniez.c_str(), currency.c_str(), HD());
                } else {
                    printf("%s[" ADDRFMT "] " RED "(%02x) %s unknown %s (moniez)" NORMAL "%s\n",
                        spaces, ADDR, instr, op, currency.c_str(), HDE());
                    NEXT_INSTR();
                }
                break;
            }
            case 0x86: // run sub-instr, access audio, volume according to darkmoon
            case 0x87: // speed according to darkmoon
            {
                const char* what = (instr==0x86) ? "volume" : "speed";
                bool ok=true;
                std::string v = parse_sub(scriptaddr, &ok);
                if (ok) {
                    printf("%s[" ADDRFMT "] (%02x) SET AUDIO %s to %s%s\n",
                        spaces, ADDR, instr, what, v.c_str(), HD());
                } else {
                    printf("%s[" ADDRFMT "] " RED "(%02x) SET AUDIO %s to %s" NORMAL "%s\n",
                        spaces, ADDR, instr, what, v.c_str(), HDE());
                    NEXT_INSTR();
                }
                break;
            }
            case 0x88: // clear ring menu item list according to darkmoon
                printf("%s[" ADDRFMT "] (%02x) Clear shopping ring%s\n",
                    spaces, ADDR, instr, HD());
                break;
            case 0x89: // add item to shop ring menu according to darkmoon
            {
                bool ok = true;
                std::string item = parse_sub(scriptaddr,&ok);
                std::string price = ok?parse_sub(scriptaddr,&ok):"?";
                if (ok) {
                    printf("%s[" ADDRFMT "] (%02x) Add item %s priced %s to shop menu%s\n",
                        spaces, ADDR, instr, item.c_str(), price.c_str(), HD());
                } else {
                    printf("%s[" ADDRFMT "] " RED "(%02x) Add item %s priced %s to shop menu" NORMAL "%s\n",
                        spaces, ADDR, instr, item.c_str(), price.c_str(), HD());
                    NEXT_INSTR();
                }
                break;
            }
            case 0x8a: // move shop ring to entity according to darkmoon
            {
                bool ok = true;
                std::string entity = parse_sub(scriptaddr, &ok);
                if (ok) {
                    printf("%s[" ADDRFMT "] (%02x) Move shop menu to %s%s\n",
                        spaces, ADDR, instr, entity.c_str(), HD());
                } else {
                    printf("%s[" ADDRFMT "] " RED "(%02x) Move shop menu to %s" NORMAL "%s\n",
                        spaces, ADDR, instr, entity.c_str(), HD());
                    NEXT_INSTR();
                }
                break;
            }
            case 0x8c: // open save menu according to darkmoon
                val16 = read16(scriptaddr); scriptaddr+=2; // no clue, pointer to room name?
                printf("%s[" ADDRFMT "] (%02x) Show save menu 0x%04x%s\n",
                    spaces, ADDR, instr, val16, HD());
                break;
            case 0x8d: // start/stop screen shaking
                type = read8(scriptaddr++);
                printf("%s[" ADDRFMT "] (%02x) %02x %s screen shaking%s\n",
                    spaces, ADDR, instr, type, type==0 ? "Stop" : "Start", HD());
                break;
            case 0x8e: // RJMP if moniez>=amount according to darkmoon, like 0a but sub-instr for amount
            case 0x8f: // if moniez<amount, like 0b but sub-instr for amount
            {
                const char* op = (instr==0x8e) ? GE : LT;
                bool ok = true;
                std::string currency = sub2currency(parse_sub(scriptaddr,&ok));
                std::string v = ok?parse_sub(scriptaddr,&ok):"?";
                int16_t jmp = (int16_t)read16(scriptaddr); scriptaddr+=2;
                signed dst = (signed)scriptaddr-scriptstart+jmp;
                if (ok) {
                    printf("%s[" ADDRFMT "] (%02x) IF %s (moniez) %s %s THEN SKIP %d (to +x%02x)%s\n",
                        spaces, ADDR, instr, currency.c_str(), op, v.c_str(), jmp, DST, HD());
                    offs.push_back(dst);
                    if (dst>maxoff) maxoff = dst;
                } else {
                    printf("%s[" ADDRFMT "] " RED "(%02x) IF %s (moniez) %s ...? THEN SKIP ...?" NORMAL "%s\n",
                        spaces, ADDR, instr, currency.c_str(), op, HDE());
                    NEXT_INSTR();
                }
                break;
            }
            case 0x91: // set brightness
            {
                bool ok=true;
                std::string br = parse_sub(scriptaddr, &ok);
                if (ok) {
                    printf("%s[" ADDRFMT "] (%02x) Sets brightness to %s%s\n",
                        spaces, ADDR, instr, br.c_str(), HD());
                } else {
                    printf("%s[" ADDRFMT "] " RED "(%02x) Sets brightness to %s" NORMAL "%s\n",
                        spaces, ADDR, instr, br.c_str(), HDE());
                    NEXT_INSTR();
                }
                break;
            }
            case 0x92:
            case 0x93:
            case 0x94:
            case 0x95:
            case 0xbb: // damage showing number according to darmoon
            {
                const char* info = (instr==0xbb)?"SHOWING NUMBER":(instr==0x94||instr==0x92)?"WITH ANIMATION":"";
                const char* dmgOrHeal = (instr<0x94||instr==0xbb)?"DAMAGE":"HEAL";
                bool ok = true;
                std::string entity = parse_sub(scriptaddr, &ok);
                std::string v = ok?parse_sub(scriptaddr, &ok):"?";
                if (ok) {
                    printf("%s[" ADDRFMT "] (%02x) %s %s FOR %s %s%s\n",
                        spaces, ADDR, instr, dmgOrHeal, entity.c_str(), v.c_str(), info, HD());
                } else {
                    printf("%s[" ADDRFMT "] " RED "(%02x) %s %s FOR %s %s" NORMAL "%s\n",
                        spaces, ADDR, instr, dmgOrHeal, entity.c_str(), v.c_str(), info, HDE());
                    NEXT_INSTR();
                }
                break;
            }
            case 0x97: // unknown, runs 7 sub-instrs
                {
                    // try to skip over the sub-instrs
                    bool ok = true;
                    std::string tmp;
                    for (uint8_t i=0; i<7; i++) {
                        if (!tmp.empty()) tmp += ", ";
                        tmp += parse_sub(scriptaddr, &ok);
                        if (!ok) break;
                    }
                    if (ok) {
                        printf("%s[" ADDRFMT "] " UNTRACED "(%02x) UNTRACED INSTR 0x97, 7 sub-instrs: %s" NORMAL "%s\n",
                                spaces, ADDR, instr, tmp.c_str(), HD());
                    } else {
                        printf("%s[" ADDRFMT "] " RED "(%02x) UNKNOWN INSTR 0x97, 7 sub-instrs: %s" NORMAL "%s\n",
                                spaces, ADDR, instr, tmp.c_str(), HDE());
                        NEXT_INSTR();
                    }
                }
                break;
            case 0x98: // switch character
            {
                bool ok = true;
                std::string entity = parse_sub(scriptaddr, &ok);
                if (ok) {
                    printf("%s[" ADDRFMT "] (%02x) SWITCH CHAR TO %s%s\n",
                            spaces, ADDR, instr, entity.c_str(), HD());
                } else {
                    printf("%s[" ADDRFMT "] " RED "(%02x) SWITCH CHAR TO %s" NORMAL "%s\n",
                            spaces, ADDR, instr, entity.c_str(), HDE());
                    NEXT_INSTR();
                }
                break;
            }
            case 0x99: // Enter Mode 7 Worldmap on Windwalker according to darkmoon
            {
                bool ok = true;
                std::string arg1 = parse_sub(scriptaddr,&ok);
                std::string arg2 = ok?parse_sub(scriptaddr,&ok):"?";
                std::string arg3 = ok?parse_sub(scriptaddr,&ok):"?";
                std::string arg4 = ok?parse_sub(scriptaddr,&ok):"?";
                std::string arg5 = ok?parse_sub(scriptaddr,&ok):"?";
                std::string arg6 = ok?parse_sub(scriptaddr,&ok):"?";
                if (ok) {
                    printf("%s[" ADDRFMT "] " UNTRACED "(%02x) WINDWALK args %s %s %s %s %s %s" NORMAL "%s\n",
                        spaces, ADDR, instr, arg1.c_str(),arg2.c_str(),arg3.c_str(),arg4.c_str(),arg5.c_str(),arg6.c_str(), HD());
                } else {
                    printf("%s[" ADDRFMT "] " RED "(%02x) WINDWALK  args %s %s %s %s %s ..." NORMAL "%s\n",
                        spaces, ADDR, instr, arg1.c_str(),arg2.c_str(),arg3.c_str(),arg4.c_str(),arg5.c_str(), HDE());
                    NEXT_INSTR();
                }
                break;
            }
            case 0x9a: // change font according to darkmoon
            {
                bool ok = true;
                std::string v = parse_sub(scriptaddr,&ok);
                if (ok) {
                    printf("%s[" ADDRFMT "] " UNTRACED "(%02x) CHANGE FONT TO %s" NORMAL "%s\n",
                        spaces, ADDR, instr, v.c_str(), HD());
                } else {
                    printf("%s[" ADDRFMT "] " RED "(%02x) CHANGE FONT TO %s" NORMAL "%s\n",
                        spaces, ADDR, instr, v.c_str(), HDE());
                    NEXT_INSTR();
                }
                break;
            }
            case 0x9b: // unknown, seems to run one sub-instr, dealloc according to darkmoon
            {
                bool ok = true;
                std::string entity = parse_sub(scriptaddr, &ok);
                if (ok) {
                    printf("%s[" ADDRFMT "] (%02x) DESTROY/DEALLOC ENTITY %s%s\n",
                            spaces, ADDR, instr, entity.c_str(), HD());
                } else {
                    printf("%s[" ADDRFMT "] " RED "(%02x) DESTROY/DEALLOC ENTITY %s" NORMAL "%s\n",
                            spaces, ADDR, instr, entity.c_str(), HDE());
                    NEXT_INSTR();
                }
                break;
            }
            case 0x9c: // runs 1 sub-instr, decrements scripts attached to entity? according to darkmoon
                       // is this ref count so that entity can't be unloaded while used in scripts?
            {
                bool ok = true;
                std::string entity = parse_sub(scriptaddr, &ok);
                if (ok) {
                    printf("%s[" ADDRFMT "] (%02x) DECREMENT SCRIPT COUNTER FOR ENTITY %s ?%s\n",
                            spaces, ADDR, instr, entity.c_str(), HD());
                } else {
                    printf("%s[" ADDRFMT "] " RED "(%02x) DECREMENT SCRIPT COUNTER FOR ENTITY %s ?" NORMAL "%s\n",
                            spaces, ADDR, instr, entity.c_str(), HDE());
                    NEXT_INSTR();
                }
                break;
            }
            case 0x9e: // alchemy attack according to darkmoon
            case 0xac: // alchemy attack according to darkmoon, exclude dog if dead
            {
                const char* info = (instr==0xac)?" if alive":"";
                bool ok = true;
                std::string entity = parse_sub(scriptaddr,&ok);
                std::string spell = ok?parse_sub(scriptaddr,&ok):"?";
                std::string power = ok?parse_sub(scriptaddr,&ok):"?";
                std::string targets;
                std::string target;
                do {
                    target = ok?parse_sub(scriptaddr,&ok):"?";
                    char* end = nullptr;
                    long tmp = strtol(target.c_str(), &end, 0);
                    if (tmp==0 && *end==0) break; // zero = end of list
                    if (!targets.empty() && !target.empty()) targets += ", ";
                    targets += target;
                } while (ok && !target.empty());
                if (ok) {
                    printf("%s[" ADDRFMT "] (%02x) %s CASTS SPELL %s POWER %s ON %s%s%s\n",
                           spaces, ADDR, instr, entity.c_str(), spell.c_str(), power.c_str(), targets.c_str(), info, HD());
                } else {
                    printf("%s[" ADDRFMT "] " RED "(%02x) %s CASTS SPELL %s POWER %s ON %s%s" NORMAL "%s\n",
                           spaces, ADDR, instr, entity.c_str(), spell.c_str(), power.c_str(), targets.c_str(), info, HD());
                    NEXT_INSTR();
                }
                break;
            }
            case 0x9f: // prepare currecny display according to darkmoon
                printf("%s[" ADDRFMT "] (%02x) PREPARE CURRENCY DISPLAY%s\n",
                    spaces, ADDR, instr, HD());
            	break;
            case 0xa0: // show currecny amount according to darkmoon
                printf("%s[" ADDRFMT "] (%02x) SHOW CURRENCY AMOUNT%s\n",
                    spaces, ADDR, instr, HD());
                break;
            case 0xa1: // hide currency display according to darkmoon
                printf("%s[" ADDRFMT "] (%02x) HIDE CURRENCY DISPLAY%s\n",
                    spaces, ADDR, instr, HD());
                break;
            case 0xa2: // spawn npc (according to darkmoon x/y are 2 sub-instrs)
            {
                uint16_t npc = read16(scriptaddr); scriptaddr+=2;
                uint16_t flags = read16(scriptaddr); scriptaddr+=2;
                bool ok = true;
                std::string x = parse_sub(scriptaddr, &ok);
                std::string y = ok?parse_sub(scriptaddr, &ok):"?";
                if (ok) {
                    printf("%s[" ADDRFMT "] (%02x) SPAWN NPC 0x%04x" GT GT "1, flags 0x%02x, x:%s, y:%s%s\n",
                            spaces, ADDR, instr, npc, flags, x.c_str(), y.c_str(), HD());
                } else {
                    printf("%s[" ADDRFMT "] " RED "(%02x) SPAWN NPC 0x%04x" GT GT "1, flags 0x%02x, x:%s, y:%s" NORMAL "%s\n",
                            spaces, ADDR, instr, npc, flags, x.c_str(), y.c_str(), HDE());
                    NEXT_INSTR();
                }
                break;
            }
            case CALL_SUB: // 0xa3, call sub script
            {
                type = read8(scriptaddr++);
#ifdef AUTO_DISCOVER_SCRIPTS
                bool exists = false;
                for (const auto& s:globalscripts) if (s.first == type) { exists=true; break; }
                if (!exists) {
                    strings.push_back(strdup(("Unnamed Global script " + u8val2str(type)).c_str()));
                    globalscripts.push_back(std::make_pair(type, strings.back()));
                }
#endif
                const char* scriptname = globalscript2name(type, nullptr);
                if (scriptname) {
                    printf("%s[" ADDRFMT "] (%02x) CALL \"%s\" (0x%02x)%s\n",
                           spaces, ADDR, instr, scriptname, (unsigned)type, HD());
                } else {
                    printf("%s[" ADDRFMT "] (%02x) CALL SCRIPT 0x%02x%s\n",
                           spaces, ADDR, instr, (unsigned)type, HD());
                }
                if (type==0x39) // loot nature
                    loot.dataset |= LootData::DataSet::callSniff;
                else if (type==0x3a) // loot gourd
                    loot.dataset |= LootData::DataSet::callGourd;
                break;
            }
            case CALL_16BIT: // 0xa4, some call instr with 16bit addr
            {
                val16 = read16(scriptaddr); scriptaddr+=2;
#ifdef AUTO_DISCOVER_SCRIPTS
                bool exists = false;
                for (const auto& s:npcscripts) if (s.first == val16) { exists=true; break; }
                if (!exists) {
                    strings.push_back(strdup(("Unnamed Short script " + u16val2str(val16)).c_str()));
                    npcscripts.push_back(std::make_pair(val16, strings.back()));
                }
#endif
                //uint32_t mapscriptptr = 0x928000 + read16(0x928000);
                printf("%s[" ADDRFMT "] (%02x) CALL 0x%04x -" GT " 0x%06x%s\n",
                    spaces, ADDR, instr, val16,
                    script2romaddr(read24(0x928000 + read16(0x928000) + val16)), HD());
                break;
            }
            case CALL_8BIT_NEG: // 0xa5, looks like a call with a negative 8bit offset
            // 8cd35c
            {
                int16_t off = (int16_t)read8(scriptaddr++) - 0x100; // this seems to be 8bit negative address ( LDA [$82], ORA #$FF00 )
                unsigned dst = (scriptstart+instroff+off);
                if (!(dst&0x8000)) dst-=0x8000; // is this also true for a5 or only for a6?
                    
#if defined AUTO_DISCOVER_SCRIPTS && !defined INLINE_RCALLS
                bool exists = false;
                for (const auto& s:absscripts) if (s.first == addr) { exists=true; break; }
                if (!exists) {
                    strings.push_back(strdup(("Unnamed ABS script " + u24val2str(addr)).c_str()));
                    absscripts.push_back(std::make_pair(addr, strings.back()));
                }
#endif
                printf("%s[" ADDRFMT "] (%02x) RCALL %d (to 0x%06x): %s%s\n",
                    spaces, ADDR, instr, off, dst, absscript2name(dst), HD());
#if defined INLINE_RCALLS
                printscript((std::string(spaces)+"  ").c_str(), buf, dst, len, btrigger, mapid, scriptid, x1, y1, x2, y2, depth+1);
#endif
                break;
            }
            case CALL_16BIT_REL: // 0xa6, looks like a call with a relative 16bit offset
            // 8cd340
            {
                int16_t off = (int16_t)read16(scriptaddr); scriptaddr+=2; // this seems to be some address, not sure
                unsigned dst = (scriptstart+instroff+off);
                if (!(dst&0x8000) && off<0) dst-=0x8000;
                else if (!(dst&0x8000)) dst+=0x8000;

#if defined AUTO_DISCOVER_SCRIPTS && !defined INLINE_RCALLS
                bool exists = false;
                for (const auto& s:absscripts) if (s.first == addr) { exists=true; break; }
                if (!exists) {
                    strings.push_back(strdup(("Unnamed ABS script " + u24val2str(addr)).c_str()));
                    absscripts.push_back(std::make_pair(addr, strings.back()));
                }
#endif
                printf("%s[" ADDRFMT "] (%02x) RCALL %4d (to 0x%06x): %s%s\n",
                    spaces, ADDR, instr, off, dst, absscript2name(dst), HD());
#if defined INLINE_RCALLS
                printscript((std::string(spaces)+"  ").c_str(), buf, dst, len, btrigger, mapid, scriptid, x1, y1, x2, y2, depth+1);
#endif
                break;
            }
            case SLEEP_8BIT:    // 0xa7, sleep/delay frames (8-bit)
            case SLEEP_16BIT:   // 0xa8, sleep/delay frames (16-bit)
                if (instr==0xa7) val16 = read8(scriptaddr++);
                else { val16 = read16(scriptaddr); scriptaddr+=2; }
                printf("%s[" ADDRFMT "] (%02x) SLEEP %d TICKS%s\n",
                    spaces, ADDR, instr, (unsigned)(val16-1), HD());
                break;
            case 0xa9: // unknown, seems to run 2 subinstrs. prime examples A9 D0 E2 and A9 D1 E2
            {      // first one gets entity pointer, second one indicates how to modify bitfield (i.e viewing direction) according to darkmoon
                bool ok = true;
                std::string entity = parse_sub(scriptaddr, &ok);
                std::string bits = ok?parse_sub(scriptaddr, &ok):"?";
                if (ok) {
                    printf("%s[" ADDRFMT "] " UNTRACED "(%02x) UNTRACED INSTR modifies entity %s bits %s" NORMAL "%s\n",
                        spaces, ADDR, instr, entity.c_str(), bits.c_str(), HD());
                } else {
                    printf("%s[" ADDRFMT "] " RED "(%02x) UNKNOWN INSTR entity %s bits %s" NORMAL "%s\n",
                        spaces, ADDR, instr, entity.c_str(), bits.c_str(), HDE());
                    NEXT_INSTR();
                }
                break;
            }
            case 0xaa: // according to darkmoon
                printf("%s[" ADDRFMT "] (%02x) Clear boy and dog statuses%s\n",
                    spaces, ADDR, instr, HD());
                break;
            case 0xab: // according to darkmoon
                printf("%s[" ADDRFMT "] (%02x) Reset game%s\n",
                    spaces, ADDR, instr, HD());
                unknowninstrs--;
                NEXT_INSTR();
                break;
            case 0xad: // store two bytes << 3 to two adresses offsets to $2834
            {
                unsigned addr1 = 0x2834+read16(scriptaddr); scriptaddr+=2;
                unsigned addr2 = 0x2834+read16(scriptaddr); scriptaddr+=2;
                unsigned val1  = read8(scriptaddr++); val1<<=3;
                unsigned val2  = read8(scriptaddr++); val1<<=3;
                printwrite(spaces, ADDR, instr, addr1, val1, HD());
                printwrite(spaces, ADDR, instr, addr2, val2);
                break;
            }
            case 0xae: // unknown, modifies current script
            {
                uint8_t val1 = read8(scriptaddr++);
                uint8_t val2 = read8(scriptaddr++);
                uint8_t val3 = read8(scriptaddr++);
                uint8_t val4 = read8(scriptaddr++);
                printf("%s[" ADDRFMT "] " UNTRACED "(%02x) UNTRACED INSTR, vals %02x %02x %02x %02x modifies current script" NORMAL "%s\n",
                    spaces, ADDR, instr, val1, val2, val3, val4, HD());
                break;
            }
            case 0xaf: // b4 was traced, af not. they look very similar // TODO: trace
            case 0xb0: // same args as b2, read below // TODO: trace
            case 0xb1: // same args as b3, read below // TODO: trace
            case 0xb2: // like 0xb4 but script is byte-offset?
            case 0xb3: // like 0xb4 but script is relative 16bit?
            case 0xb4: // write multiple words from sub-instrs to $0e80+0..2*N-1 and execute absolute script (24bit) or 24bit offset??
            {
                // my guess, one of each 8, 16, 24bit RCALL, 8, 16, 24 absolute CALL // TODO: trace
                addr = 0x0e80;
                N = read8(scriptaddr++);
                bool ok = true;
                std::string tmp;
                for (uint8_t i=0; i<N; i++) {
                    if (!tmp.empty()) tmp += ", ";
                    tmp += parse_sub(scriptaddr, &ok);
                    if (!ok) break;
                }
                if (ok) {
                    uint32_t index=0; const char* scripttype; const char* name;
                    if (instr == 0xb0) { // 8bit global script call?
                        index = read8(scriptaddr++);
#ifdef AUTO_DISCOVER_SCRIPTS
                        bool exists = false;
                        for (const auto& s:globalscripts) if (s.first == index) { exists=true; break; }
                        if (!exists) {
                            strings.push_back(strdup(("Unnamed Global script " + u8val2str((uint8_t)index)).c_str()));
                            globalscripts.push_back(std::make_pair((uint8_t)index, strings.back()));
                        }
#endif
                        addr = index;
                        name = globalscript2name((uint8_t)index);
                        scripttype = "Global (8bit)";
                    } else if (instr == 0xb1) { // 16bit npc script call?
                        index = read16(scriptaddr); scriptaddr+=2;
#ifdef AUTO_DISCOVER_SCRIPTS
                        bool exists = false;
                        for (const auto& s:npcscripts) if (s.first == index) { exists=true; break; }
                        if (!exists) {
                            strings.push_back(strdup(("Unnamed Short script " + u16val2str((uint16_t)index)).c_str()));
                            npcscripts.push_back(std::make_pair((uint16_t)index, strings.back()));
                        }
#endif
                        addr = index;
                        name = npcscript2name((uint16_t)index);
                        scripttype = "Short/NPC (16bit)";
                    } else if (instr == 0xb2) { // 8bit relative call? is this right?
                        index = read8(scriptaddr++);
                        addr = scriptstart+instroff-0x100+index;
                        if (!(addr&0x8000)) addr-=0x8000;
#ifdef AUTO_DISCOVER_SCRIPTS
                        bool exists = false;
                        for (const auto& s:absscripts) if (s.first == addr) { exists=true; break; }
                        if (!exists) {
                            strings.push_back(strdup(("Unnamed ABS script " + u24val2str(addr)).c_str()));
                            absscripts.push_back(std::make_pair(addr, strings.back()));
                        }
#endif
                        name = absscript2name(addr);
                        scripttype = "Relative (8bit)";
                        // TODO: inline rcall
                    } else if (instr == 0xb3) { // 16bit relative call? is this right?
                        index = read16(scriptaddr); scriptaddr+=2;
                        addr = scriptstart+instroff-0x10000+index;
                        if (!(addr&0x8000)) addr-=0x8000;
#ifdef AUTO_DISCOVER_SCRIPTS
                        bool exists = false;
                        for (const auto& s:absscripts) if (s.first == addr) { exists=true; break; }
                        if (!exists) {
                            strings.push_back(strdup(("Unnamed ABS script " + u24val2str(addr)).c_str()));
                            absscripts.push_back(std::make_pair(addr, strings.back()));
                        }
#endif
                        name = absscript2name(addr);
                        scripttype = "Relative (16bit)";
                        // TODO: inline rcall
                    } else {
                        index = read24(scriptaddr); scriptaddr+=3;
                        scripttype = "Absolute (24bit)";
                        addr = script2romaddr(index);
#ifdef AUTO_DISCOVER_SCRIPTS
                        bool exists = false;
                        for (const auto& s:absscripts) if (s.first == addr) { exists=true; break; }
                        if (!exists) {
                            strings.push_back(strdup(("Unnamed ABS script " + u24val2str(addr)).c_str()));
                            absscripts.push_back(std::make_pair(addr, strings.back()));
                        }
#endif
                        name = absscript2name(addr);
                    }
                    printf("%s[" ADDRFMT "] (%02x) CALL %s script 0x%02x (\"%s\")\n", spaces, ADDR, instr, scripttype, addr, name);
                    printf("%s                WITH %d ARGS %s%s\n", spaces, N, tmp.c_str(), HD());
                } else {
                    printf("%s[" ADDRFMT "] " RED "(%02x) WRITE TO " /*"$%02x+0..%02x (args?)"*/ "ARGS" " from %d sub-instrs:\n",
                        spaces, ADDR, instr, /*addr, (unsigned)N*2-1,*/ N);
                    printf("%s            %s\n", spaces, tmp.c_str());
                    printf("%s            and CALL ...?" NORMAL "%s\n", spaces, HDE());
                    NEXT_INSTR();
                }
                break;
            }
            case 0xb5: // "Draw Lightning/Reveal Hidden NPC????" according to darkmoon
            {
                bool ok = true;
                std::string arg1 = parse_sub(scriptaddr,&ok);
                std::string arg2 = ok?parse_sub(scriptaddr,&ok):"?";
                std::string arg3 = ok?parse_sub(scriptaddr,&ok):"?";
                std::string arg4 = ok?parse_sub(scriptaddr,&ok):"?";
                std::string arg5 = ok?parse_sub(scriptaddr,&ok):"?";
                std::string arg6 = ok?parse_sub(scriptaddr,&ok):"?";
                std::string arg7 = ok?parse_sub(scriptaddr,&ok):"?";
                std::string arg8 = ok?parse_sub(scriptaddr,&ok):"?";
                if (ok) {
                    printf("%s[" ADDRFMT "] " UNTRACED "(%02x) REVEAL ENTITY?? args %s %s %s %s %s %s %s %s" NORMAL "%s\n",
                        spaces, ADDR, instr, arg1.c_str(),arg2.c_str(),arg3.c_str(),arg4.c_str(),arg5.c_str(),arg6.c_str(),arg7.c_str(),arg8.c_str(), HD());
                } else {
                    printf("%s[" ADDRFMT "] " RED "(%02x) REVEAL ENTITY?? args %s %s %s %s %s %s %s ..." NORMAL "%s\n",
                        spaces, ADDR, instr, arg1.c_str(),arg2.c_str(),arg3.c_str(),arg4.c_str(),arg5.c_str(),arg6.c_str(),arg7.c_str(), HDE());
                    NEXT_INSTR();
                }
                break;
            }
            case 0xb6: // "tile flashing", 4 sub-instr,s according to darkmoon
            {
                bool ok = true;
                std::string arg1 = parse_sub(scriptaddr, &ok);
                std::string arg2 = ok?parse_sub(scriptaddr, &ok):"?";
                std::string arg3 = ok?parse_sub(scriptaddr, &ok):"?";
                std::string arg4 = ok?parse_sub(scriptaddr, &ok):"?";
                if (ok) {
                    printf("%s[" ADDRFMT "] (%02x) START TILE FLASHING %s %s %s %s%s\n",
                        spaces, ADDR, instr, arg1.c_str(),arg2.c_str(),arg3.c_str(),arg4.c_str(), HD());
                } else {
                    printf("%s[" ADDRFMT "] " RED "(%02x) START TILE FLASHING %s %s %s %s" NORMAL "%s\n",
                        spaces, ADDR, instr, arg1.c_str(),arg2.c_str(),arg3.c_str(),arg4.c_str(), HDE());
                    NEXT_INSTR();
                }
                break;
            }
            case 0xb7: // "stop tile flashing", 1 sub-instr, according to darkmoon
            {
                bool ok = true;
                std::string arg1 = parse_sub(scriptaddr, &ok);
                if (ok) {
                    printf("%s[" ADDRFMT "] (%02x) STOP TILE FLASHING %s%s\n",
                        spaces, ADDR, instr, arg1.c_str(), HD());
                } else {
                    printf("%s[" ADDRFMT "] " RED "(%02x) STOP TILE FLASHING %s" NORMAL "%s\n",
                        spaces, ADDR, instr, arg1.c_str(), HDE());
                    NEXT_INSTR();
                }
                break;
            }
            case 0xba: // code similar to 3c, but arg1 is only 1B and arg2 is 0
            {          // (also) load NPC according to darkmoon
                    
                uint8_t  arg1 = read8(scriptaddr++);
                uint8_t  arg3 = read8(scriptaddr++);
                uint8_t  arg4 = read8(scriptaddr++);
                printf("%s[" ADDRFMT "] (%02x) LOAD NPC %02x at %02x %02x%s\n",
                    spaces, ADDR, instr, arg1, arg3, arg4, HD());
                break;
            }
            case 0xbc:
                printf("%s[" ADDRFMT "] (%02x) Stop/disable boy (and SELECT button)%s\n",
                    spaces, ADDR, instr, HD());
                break;
            case 0xbd: // hope this is right
                printf("%s[" ADDRFMT "] (%02x) BOY = Player controlled%s\n",
                    spaces, ADDR, instr, HD());
                break;
            case 0xbe:
                printf("%s[" ADDRFMT "] (%02x) Stop/disable doggo (and SELECT button)%s\n",
                    spaces, ADDR, instr, HD());
                break;
            case 0xbf:
                printf("%s[" ADDRFMT "] (%02x) DOG = Player controlled%s\n",
                    spaces, ADDR, instr, HD());
                break;
            case 0xc0: // see 2A
                printf("%s[" ADDRFMT "] (%02x) BOY+DOG = STOPPED%s\n",
                    spaces, ADDR, instr, HD());
                break;
            case 0xc1:
                printf("%s[" ADDRFMT "] (%02x) BOY+DOG = Player controlled%s\n",
                    spaces, ADDR, instr, HD());
                break;
            case 0xc2: // unknown, reads 3 bytes, accesses rom, writes to some object
                       // adds npc to conditional spawn list according to darkmoon
            {
                uint8_t a = read8(scriptaddr++); uint8_t b = read8(scriptaddr++); uint8_t c = read8(scriptaddr++);
                printf("%s[" ADDRFMT "] (%02x) Add NPC 0x%02x spawner at 0x%02x,0x%02x" NORMAL "%s\n",
                    spaces, ADDR, instr, a, b, c, HD());
                break;
            }
            default:
                printf("%s[" ADDRFMT "] " RED "(%02x) UNKNOWN INSTR" NORMAL "%s\n",
                    spaces, ADDR, instr, HDE());
#ifdef DEBUG_OFFS
if (maxoff<=(signed)instroff) printf("END\n");
offs.sort();
for (auto off: offs) {
    if (off>(signed)instroff) {
        scriptaddr = scriptstart+off;
        printf("-> %x\n", scriptaddr);
        break;
    }
}
#endif
                NEXT_INSTR();
                break;
        }
    }
    
    if (btrigger && ((loot.dataset & LootData::DataSet::callSniff) != LootData::DataSet::none))
    {
        bool exists=false;
        for (const auto& tmp: sniffs) {
            if (loot.same_source(tmp)) {
                exists = true;
                break;
            }
        }
        if (!exists) sniffs.push_back(loot);
    }
    else if (btrigger && ((loot.dataset & LootData::DataSet::callGourd) != LootData::DataSet::none))
    {
        bool exists=false;
        for (const auto& tmp: gourds) {
            if (loot.same_source(tmp)) {
                exists = true;
                break;
            }
        }
        if (!exists) gourds.push_back(loot);
    }
}

int main(int argc, char** argv)
{
    if (argc<2 || !argv[1] || !argv[1][0] || (strcmp(argv[1], "-b")==0 && argc<3)) {
        fprintf(stderr, "Usage: %s [-b] <rom file>\n\n-b: batch mode (non-interactive)\n", argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "-b")==0) batch = true;

    uint8_t* buf = NULL;
    FILE* f = fopen(argv[batch?2:1], "rb");
    if (!f) die("Could not open input file!\n");
    fseek(f, 0L, SEEK_END);
    size_t len = ftell(f);
    if (len == 3145728+512 || len == 4194304+512) {
        fseek(f, 512L, SEEK_SET);
        len -= 512;
    } else if (len == 3145728 || len == 4194304) {
        fseek(f, 0L, SEEK_SET);
    } else {
        die("ROM has to be 3MB or grown to 4MB (with or without header)!\n");
    }
    buf = (uint8_t*)malloc(len);
    if (!buf) die("Out of memory!\n");
    if (fread(buf, 1, len, f) != len) die("Could not read input file!\n");
    fclose(f); f = nullptr;
    
    bool skip_scriptless = true; // TODO parse command line args, NOT --all
    (void)skip_scriptless; // remove warning, usage of this flag depends on defines
    size_t total_mscripts=0;
    size_t total_bscripts=0;
    size_t total_rooms=0;
    size_t total_rooms_with_mscripts=0;
    size_t total_rooms_with_bscripts=0;
    size_t total_invalid_script_sections=0;
    
    uint32_t mapscriptptr = 0x928000 + read16(0x928000);
    uint32_t globalscriptptr = 0x928000 + read16(0x928000) + read16(0x928008);
    
    printf(START);
    printf("Map script adresses are located at 0x%06x\n", mapscriptptr);
    printf("Global/callable script adresses at 0x%06x\n", globalscriptptr);
    
#ifdef INCLUDE_UNKNOWN_MAPS
    uint32_t max_mapid = 0;
    for (const auto& pair: maps) {
        if (pair.first>max_mapid) max_mapid = pair.first;
    }
    for (const auto& pair: maps) {
        if (pair.first==max_mapid) {
            printf("Last map is 0x%02x: %s\n", pair.first, pair.second);
            break;
        }
    }
    max_mapid |= 0x0f;
    printf("Adding unknown map IDs up to 0x%02x\n", max_mapid);
    for (unsigned i=0; i<=max_mapid; i++) {
        if (read24(MAP_LIST + (i<<2)) > 0xafffff) continue; // invalid data pointer
        bool match = false;
        for (const auto& pair: maps) {
            if (pair.first==(uint8_t)i) {
                match = true;
                break;
            }
        }
        // TODO: get name from ROM
        if (!match) maps.push_back(std::make_pair((uint8_t)i, "Unknown"));
    }
#endif
    
    for (auto& pair: maps) {
        uint16_t offset = ((uint16_t)pair.first) << 2;
        uint32_t dataptr = read24(MAP_LIST + offset);
        if (dataptr > 0xafffff) {
            printf(HEADING "[0x%02x] " HEADING_TEXT "%s" HEADING_END " at 0x%06x" NORMAL "\n", (unsigned)pair.first, pair.second, (unsigned)MAP_LIST + offset);
            printf("  " RED "data at 0x%06x does not exist" NORMAL "\n\n", (unsigned)dataptr);
            continue;
        }
        // step-on-trigger-scripts
        uint32_t mscriptlistptr = dataptr+0x0d+2;
        if (!addr_valid(mscriptlistptr)) die("Unsupported ROM");
        uint16_t mscriptlistlen = read16(dataptr+0x0d);
        // b-trigger-scripts
        uint32_t bscriptlistptr = dataptr+0x0d+2+mscriptlistlen+2;
        if (!addr_valid(bscriptlistptr)) die("Unsupported ROM");
        uint16_t bscriptlistlen = read16(dataptr+0x0d+2+mscriptlistlen);
        // enter script
        uint32_t escriptptr  = 0x92801b + 5UL * pair.first;
        if (!addr_valid(escriptptr)) die("Unsupported ROM");
        uint32_t escriptaddr = script2romaddr(read24(escriptptr));
        
        total_rooms++;
#ifndef SHOW_ENTER_SCRIPTS
        if (!bscriptlistlen && !mscriptlistlen && skip_scriptless) continue;
#endif
        
        printf(HEADING "[0x%02x] " HEADING_TEXT "%s" HEADING_END " at 0x%06x" NORMAL "\n", (unsigned)pair.first, pair.second, (unsigned)MAP_LIST + offset);
        printf("  data at 0x%06x\n", (unsigned)dataptr);
        
        printf("  enter script at 0x%06x => 0x%06x%s\n", (unsigned)escriptptr, (unsigned)escriptaddr,
#ifdef SHOW_ENTER_SCRIPTS
        "");
        printscript("      ", buf, escriptaddr, len, false, pair.first);
#else
        " omitted");
#endif
        
        printf("  step-on scripts at 0x%06x, len=0x%04x (%u entries)\n",
            (unsigned)mscriptlistptr,
            (unsigned)mscriptlistlen, (unsigned)mscriptlistlen/6);
        if (mscriptlistlen == 0) {
            printf("    " GREEN "has no scripts." NORMAL "\n");
        } else if (mscriptlistlen % 6) {
            printf("    " RED   "probably invalid, len%%6 != 0." NORMAL "\n");
            total_invalid_script_sections++;
        } else if (mscriptlistlen>600) {
            printf("    " RED   "probably invalid, more than 100 entries." NORMAL "\n");
            total_invalid_script_sections++;
        } else {
            total_rooms_with_mscripts++;
            for (uint16_t pos=0; pos<mscriptlistlen; pos+=6) {
                total_mscripts++;
#ifdef SHOW_STEP_ON_SCRIPTS
                uint8_t y1 = read8(mscriptlistptr + pos + 0);
                uint8_t x1 = read8(mscriptlistptr + pos + 1);
                uint8_t y2 = read8(mscriptlistptr + pos + 2);
                uint8_t x2 = read8(mscriptlistptr + pos + 3);
                uint16_t scriptid = read16(mscriptlistptr + pos + 4);
                uint32_t scriptaddr = script2romaddr(read24(mapscriptptr + scriptid));
                printf("    [%02x,%02x:%02x,%02x] = (id:%x => (%x@%x) => addr:0x%06x)\n",
                    x1,y1, x2,y2, scriptid, 
                    read24(mapscriptptr + scriptid), mapscriptptr + scriptid,
                    scriptaddr);
                if (pos+6<mscriptlistlen && read16(mscriptlistptr + pos + 10)==scriptid) continue; // next trigger uses the same script
                printscript("      ", buf, scriptaddr, len, false, pair.first, scriptid, x1,y1, x2,y2);
#endif
            }
        }
            
            
        printf("  B trigger scripts at 0x%06x, len=0x%04x (%u entries)\n",
            (unsigned)bscriptlistptr,
            (unsigned)bscriptlistlen, (unsigned)bscriptlistlen/6);
        if (bscriptlistlen == 0) {
            printf("    " GREEN "has no scripts." NORMAL "\n");
        } else if (bscriptlistlen % 6) {
            printf("    " RED   "probably invalid, len%%6 != 0." NORMAL "\n");
            total_invalid_script_sections++;
        } else if (bscriptlistlen>600) {
            printf("    " RED   "probably invalid, more than 100 entries." NORMAL "\n");
            total_invalid_script_sections++;
        } else {
            total_rooms_with_bscripts++;
            for (uint16_t pos=0; pos<bscriptlistlen; pos+=6) {
                total_bscripts++;
#ifdef SHOW_B_TRIGGER_SCRIPTS
                uint8_t y1 = read8(bscriptlistptr + pos + 0);
                uint8_t x1 = read8(bscriptlistptr + pos + 1);
                uint8_t y2 = read8(bscriptlistptr + pos + 2);
                uint8_t x2 = read8(bscriptlistptr + pos + 3);
                uint16_t scriptid = read16(bscriptlistptr + pos + 4);
                uint32_t scriptaddr = script2romaddr(read24(mapscriptptr + scriptid));
                printf("    [%02x,%02x:%02x,%02x] = (id:%x => (%x@%x) => addr:0x%06x)\n",
                    x1,y1, x2,y2, scriptid, 
                    read24(mapscriptptr + scriptid), mapscriptptr + scriptid,
                    scriptaddr);
                if (pos+6<bscriptlistlen && read16(bscriptlistptr + pos + 10)==scriptid) continue; // next trigger uses the same script
                printscript("      ", buf, scriptaddr, len, true, pair.first, scriptid, x1,y1, x2,y2);
#endif
            }
        }
        printf("\n");
    }
    
    size_t npcscriptsprinted=0;
    size_t globalscriptsprinted=0;
    size_t absscriptsprinted=0;

    do {
            printf(HEADING HEADING_TEXT "Known NPC and Short scripts" HEADING_END NORMAL "\n");
            size_t n=0; // TODO: use vectors instead of lists?
            for (auto& pair: npcscripts) {
                 n++;
                if (n<npcscriptsprinted) continue;
                uint32_t scriptaddr = script2romaddr(read24(mapscriptptr + pair.first));
                printf("    \"%s\" = (id:%x => addr:0x%06x)\n", pair.second, pair.first, scriptaddr);
                if ((scriptaddr&(~0xc00000)) >= len)
                    printf("      " RED " Invalid address " NORMAL "\n");
                else
                    printscript("      ", buf, scriptaddr, len);
            }
            printf("\n");

            printf(HEADING HEADING_TEXT "Known global scripts" HEADING_END NORMAL "\n");
            n = 0;
            for (auto& pair: globalscripts) {
                 n++;
                if (n<globalscriptsprinted) continue;
                uint32_t scriptaddr = script2romaddr(read24(globalscriptptr + 3*(uint16_t)pair.first));
                printf("    \"%s\" = (id:%x => addr:0x%06x)\n", pair.second, pair.first, scriptaddr);
                if ((scriptaddr&(~0xc00000)) >= len)
                    printf("      " RED " Invalid address " NORMAL "\n");
                else
                    printscript("      ", buf, scriptaddr, len);
            }
            printf("\n");

            printf(HEADING HEADING_TEXT "Known abs scripts" HEADING_END NORMAL "\n");
            n = 0;
            for (auto& pair: absscripts) {
                 n++;
                if (n<=absscriptsprinted) continue;
                if (pair.first >= 0xb00000 && len<4*1024*1024) continue; // randomizer only
                printf("    \"%s\" = (addr:0x%06x)\n", pair.second, pair.first);
                if ((pair.first&(~0xc00000)) >= len)
                    printf("      " RED " Invalid address " NORMAL "\n");
                else
                    printscript("      ", buf, pair.first, len);
            }
            printf("\n");
            npcscriptsprinted = npcscripts.size();
            globalscriptsprinted = globalscripts.size();
            absscriptsprinted = absscripts.size();
    } while (npcscriptsprinted!=npcscripts.size() ||
             globalscriptsprinted!=globalscripts.size() ||
             absscriptsprinted!=absscripts.size());
    
#ifdef FIND_EASTER_EGG
    printf("(1): script2romaddr(0x%06x) = 0x%06x (%s)\n",
        0x04c5e7, script2romaddr(0x04c5e7), (script2romaddr(0x04c5e7)==0x9bc5e7)?"ok":"nope");
    printf("(2): script2romaddr(0x%06x) = 0x%06x (%s)\n",
        0x04c5d0, script2romaddr(0x04c5d0), (script2romaddr(0x04c5d0)==0x9bc5d0)?"ok":"nope");
    // addr of caller of strange script is at ROM 0x129e03, which is part of script table 928000 + 1e03
    for (uint16_t i=0; i<=255; i++) {
        uint32_t scriptaddr = script2romaddr(read24(globalscriptptr + 3*(uint16_t)i));
        if (scriptaddr == 0x9bc5d0) {
            printf("Global script %02x calls 0x9bc5d0\n", (unsigned)i);
            break;
        }
    }
    for (uint16_t i=0; i<=0xfffe; i++) {
        uint32_t scriptaddr = script2romaddr(read24(mapscriptptr + i));
        if (scriptaddr == 0x9bc5d0) {
            printf("Map/NPC script 0x%02x calls 0x9bc5d0\n", (unsigned)i);
            break;
        }
    }
#endif

// this snippet can be used to convert between ROM and script adresses
/*
for (auto a: {0xb1e000,0x95c50d,0x95cfaa,0x95cb9a,0x9895c8,0x97cdc3}) {
    uint32_t xyz = rom2scriptaddr(a);
    printf("$%06x -> 0x%06x -> %02x %02x %02x\n", a, xyz, xyz&0xff, (xyz>>8)&0xff, (xyz>>16)&0xff);
}
*/

#ifdef PRINT_ALL_SCRIPTS
    for (uint16_t i=0; i<=0xfffe; i++) {
        uint32_t scriptaddr = script2romaddr(read24(mapscriptptr + i));
        if (scriptaddr < 0x928000) continue;
        if (scriptaddr > 0xafffff) continue;
        if (! (scriptaddr & 0x8000)) continue;
        printf("Map/NPC script 0x%04x = %06x\n", (unsigned)i, scriptaddr);
        printscript("  ", buf, scriptaddr, len);
    }
#endif

#ifdef PRINT_ALL_TEXTS
    printf(HEADING_TEXT "All Texts" HEADING_END NORMAL "\n");
    for (uint16_t i=0; i<=max_text+1; i++) {
        //if (! (0x91d000 + i)&0x8000) continue;
        std::string text = get_text(0x91d000 + i);
        if (!text.empty() && text.length()<500)
        printf("%04x %06x: %s\n", (unsigned)i, 0x91d000 + i, text.c_str());
    }
    printf("\n");
#endif

#ifdef PRINT_ALL_SNIFF_SPOTS
    printf(HEADING_TEXT "All Sniff Spots" HEADING_END NORMAL "\n");
    size_t n=0;
    for (const auto& sniff: sniffs) {
        printf("  %s\n", sniff.to_string().c_str());
        if ((sniff.check_flag != sniff.set_flag) || !(sniff.dataset&LootData::DataSet::check) || !(sniff.dataset&LootData::DataSet::set))
            printf(RED "  WARNING: multiple or missing flags" NORMAL "\n");
        n++;
    }
    printf("%u total\n", (unsigned)n);
#endif
#ifdef DUMP_ALL_SNIFF_SPOTS
    {
        FILE* f = fopen("sniff.h", "wb");
        fprintf(f, "struct sniff { uint32_t addr; uint16_t val; };\n");
        fprintf(f, "static const struct sniff sniffs[] = {");
        size_t n=0;
        for (const auto& sniff: sniffs) {
            if (n++%3 == 0)
                fprintf(f, "\n    %s", sniff.to_h().c_str());
            else
                fprintf(f, " %s", sniff.to_h().c_str());
        }
        fprintf(f, "\n};\n");
        fclose(f);
    }
#endif
#ifdef DUMP_ALL_SNIFF_FLAGS
    {
        FILE* f = fopen("sniffflags.inc", "wb");
        //fprintf(f, "static const std::map<std::pair<uint16_t, uint8_t>, const char*> sniffflags = {\n");
        for (const auto& sniff: sniffs) {
            fprintf(f, "    %s\n", sniff.to_flag().c_str());
        }
        //fprintf(f, "}\n");
        fclose(f);
    }
#endif
#ifdef PRINT_ALL_GOURDS
    printf(HEADING_TEXT "All Gourds" HEADING_END NORMAL "\n");
    size_t n=0;
    for (const auto& loot: gourds) {
        printf("  %s\n", loot.to_string().c_str());
        if ((loot.check_flag != loot.set_flag) || !(loot.dataset&LootData::DataSet::check) || !(loot.dataset&LootData::DataSet::set))
            printf(RED "  WARNING: multiple or missing flags" NORMAL "\n");
        n++;
    }
    printf("%u total\n", (unsigned)n);
#endif
#ifdef DUMP_DOGGO_CHANGE
    {
        FILE* f = fopen("doggo.h", "wb");
        fprintf(f, "//TODO:  we should also swap animations based on what doggo we have\n");
        fprintf(f, "//FIXME: $2363 is used to dynamically load doggo, but rarely used\n");
        fprintf(f, "enum doggo_consts { DOGGO_ACT0=0xba, DOGGO_ACT1=0xb2, DOGGO_ACT2=0xb6,\n"
                   "                    DOGGO_ACT3=0xb8, DOGGO_ACT4=0xbc, DOGGO_BONE=0xb4 };\n");
        fprintf(f, "static const uint8_t doggo_vals[] = {\n"
                   "    DOGGO_ACT0, DOGGO_ACT1, DOGGO_ACT2, DOGGO_ACT3, DOGGO_ACT4\n"
                   "};\n");
        fprintf(f, "struct doggo { uint32_t addr; uint8_t val; uint8_t hard; };\n");
        fprintf(f, "static const struct doggo doggos[] = {");
        size_t n=0;
        for (const auto& pair: change_doggo) {
            //assert(pair.second.inttype == IntType::subinstr1B);
            if (pair.second.inttype != IntType::subinstr1B) {
                fprintf(stderr, "WARN: unsupported change doggo at $%06x!\n",
                        pair.first);
                continue;
            }
            if (pair.second.val == 0x04) continue; // exclude bone cutscene doggo
            uint8_t byteval = pair.second.val<0x10 ?
                (pair.second.val+0xb0):(pair.second.val-0x10+0xe0);
            uint8_t hard = pair.second.mapid == 0x45 || pair.second.mapid == 0x4a;
            if (n++%4 == 0)
                fprintf(f, "\n    {0x%x,0x%02x,%d},", pair.first-0x800000, byteval, hard);
            else
                fprintf(f, " {0x%x,0x%02x,%d},", pair.first-0x800000, byteval, hard);
        }
        fprintf(f, "\n};\n");
        fclose(f);
    }
#endif

#ifndef NO_STATS
    printf("\n~ STATS ~\n");
    printf("Inspected scripts from ROM 0x%06x to 0x%06x\n", min_addr-0x800000, max_addr-0x800000);
    printf("For a total of %u instructions, hitting %u undecodable from\n"
           "     %u maps with %u step-on- and %u b-triggers,\n"
           "     %u global scripts,\n"
           "     %u npc scripts and\n"
           "     %u other scripts\n",
           instr_count, unknowninstrs,
           (unsigned)maps.size(),(unsigned)total_mscripts,(unsigned)total_bscripts,
           (unsigned)globalscripts.size(),
           (unsigned)npcscripts.size(),
           (unsigned)absscripts.size());
    printf("With %u flags and %u RAM locations documented\n\n",
        (unsigned)flags.size(), (unsigned)ram.size());

#ifdef EXTENSIVE_ROOM_STATS
    printf("Rooms with step-on script section %u/%u.\n",
        (unsigned)total_rooms_with_mscripts,
        (unsigned)total_rooms);
    printf("Rooms with b-trigger script section  %u/%u.\n",
        (unsigned)total_rooms_with_bscripts,
        (unsigned)total_rooms);
    if (total_invalid_script_sections>0)
        printf("Found %u invalid script sections\n\n",
           (unsigned)total_invalid_script_sections);
    else printf("\n");
#endif
#endif

    free(buf); buf = nullptr;
    printf(END);
    
#ifdef main
    // NOTE: we would have to remove all references of items to strings in scripts lists
    //       before we can clear it out, so we just leak the memory
#else
    for (auto s: strings) free(s);
    strings.clear();
#endif

#if (defined(WIN32) || defined(_WIN32)) && !defined(main)
    if (!batch) system("pause");
#endif

    return 0;
}
