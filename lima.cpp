#include <iostream>
#include <vector>
#include <filesystem>
#include <algorithm>
#include <fstream>
#include <locale.h>
#include <ncurses.h>
#include <unistd.h>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sys/wait.h>
#include <sys/stat.h>

namespace fs = std::filesystem;

enum class Mode { BROWSER, EDITOR, MENU, DIALOG, PROPERTIES, ABOUT };

struct MenuOption { std::string label; int id; };

class Lima {
private:
    Mode current_mode = Mode::BROWSER;
    fs::path current_path;
    bool running = true;

    // Browser/Editor State
    std::vector<std::pair<std::string, bool>> entries;
    int browser_sel = 0, browser_scroll = 0;
    std::vector<std::wstring> buffer;
    fs::path target_path; 
    int cur_y = 0, cur_x = 0, scroll_y = 0;

    // Dialog State
    std::string dialog_input = "";
    std::string dialog_title = "";
    int dialog_action = 0; // 0:Copy, 1:Move, 2:Mkdir, 3:NewFile, 4:Mount

    // Menu State
    int active_menu = 0;
    int menu_sel = 0;
    std::vector<std::vector<MenuOption>> menus = {
        { {"[N] New Folder", 0}, {"[F] New File  ", 1}, {"[P] Properties", 2}, {"[D] Delete    ", 3} },
        { {"[B] Bash Shell ", 4}, {"[M] Mount Disk ", 5} },
        { {"[A] About Lima ", 6}, {"[Q] Quit Lima  ", 7} }
    };

    void init_colors() {
        start_color();
        init_pair(1, COLOR_WHITE, COLOR_BLUE);   // Main
        init_pair(2, COLOR_BLACK, COLOR_CYAN); // Bars
        init_pair(3, COLOR_YELLOW, COLOR_BLUE); // Folders
        init_pair(4, COLOR_WHITE, COLOR_CYAN); // Selected
        init_pair(5, COLOR_RED, COLOR_CYAN);   // Alt Hotkeys
        init_pair(6, COLOR_BLACK, COLOR_WHITE); // Dialogs/Menus
    }

    std::string get_stats() {
        std::string bat = "AC";
        std::ifstream f("/sys/class/power_supply/BAT0/capacity");
        if (f) { f >> bat; bat += "%"; }
        auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::stringstream ss;
        ss << std::put_time(std::localtime(&now), "%H:%M:%S");
        return " [BAT: " + bat + "] [" + ss.str() + "] ";
    }

    void load_dir() {
        entries.clear();
        try {
            if (current_path.has_parent_path()) entries.push_back({"..", true});
            for (const auto& e : fs::directory_iterator(current_path))
                entries.push_back({e.path().filename().string(), e.is_directory()});
            std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
                if (a.first == "..") return true;
                if (a.second != b.second) return a.second > b.second;
                return a.first < b.first;
            });
        } catch (...) {}
    }

    void draw_ui() {
        int mx = getmaxx(stdscr), my = getmaxy(stdscr);
        wbkgd(stdscr, COLOR_PAIR(1));

        // 1. Header Logo & Stats
        attron(A_BOLD | COLOR_PAIR(3));
        mvprintw(0, 1, " L I M A   1.0 ");
        attroff(A_BOLD | COLOR_PAIR(3));
        std::string stats = get_stats();
        mvprintw(0, mx - stats.length(), "%s", stats.c_str());

        // 2. Alt Menu Bar
        attron(COLOR_PAIR(2));
        mvhline(1, 0, ' ', mx);
        mvprintw(1, 1, " File    Commands    Options ");
        attron(COLOR_PAIR(5) | A_BOLD);
        mvaddch(1, 2, 'F'); mvaddch(1, 10, 'C'); mvaddch(1, 22, 'O');
        attroff(COLOR_PAIR(5) | A_BOLD | COLOR_PAIR(2));

        // 3. Main Browser / Editor
        if (current_mode == Mode::EDITOR) {
            curs_set(1);
            int v_h = my - 3;
            for (int i = 0; i < v_h && (i + scroll_y) < (int)buffer.size(); ++i) {
                mvprintw(i + 2, 1, "%3d | ", i + scroll_y + 1);
                addwstr(buffer[i + scroll_y].c_str());
            }
        } else {
            curs_set(0);
            mvprintw(2, 2, "Path: %s", current_path.string().c_str());
            int v_h = my - 4;
            for (int i = 0; i < v_h && (i + browser_scroll) < (int)entries.size(); ++i) {
                int idx = i + browser_scroll;
                if (idx == browser_sel) attron(COLOR_PAIR(4));
                else if (entries[idx].second) attron(COLOR_PAIR(3));
                mvprintw(i + 3, 2, " %-4s %s ", entries[idx].second ? "DIR" : "FIL", entries[idx].first.c_str());
                attroff(COLOR_PAIR(3) | COLOR_PAIR(4));
            }
        }

        // 4. Overlays (Menus, Dialogs)
        if (current_mode == Mode::MENU) {
            int sx = (active_menu == 0) ? 1 : (active_menu == 1 ? 9 : 21);
            attron(COLOR_PAIR(6));
            for(int i=0; i<(int)menus[active_menu].size(); i++) {
                if(i == menu_sel) attron(A_REVERSE);
                mvprintw(i+2, sx, " %-15s ", menus[active_menu][i].label.c_str());
                attroff(A_REVERSE);
            }
            attroff(COLOR_PAIR(6));
        } else if (current_mode == Mode::DIALOG) {
            curs_set(1);
            attron(COLOR_PAIR(6));
            mvprintw(my/2-1, mx/2-20, " %-40s ", dialog_title.c_str());
            mvprintw(my/2, mx/2-20, " > %-37s ", dialog_input.c_str());
            attroff(COLOR_PAIR(6));
        } else if (current_mode == Mode::PROPERTIES) {
            attron(COLOR_PAIR(6));
            fs::path p = current_path / entries[browser_sel].first;
            mvprintw(my/2-3, mx/2-15, " +--- File Properties ---+ ");
            mvprintw(my/2-2, mx/2-15, " | Name: %-15s | ", p.filename().string().substr(0,15).c_str());
            auto perms = fs::status(p).permissions();
            mvprintw(my/2-1, mx/2-15, " | Perm: %-15o | ", (int)perms & 0777);
            mvprintw(my/2, mx/2-15,   " | [X] Toggle Executable | ");
            mvprintw(my/2+1, mx/2-15, " | [ESC] to Close        | ");
            mvprintw(my/2+2, mx/2-15, " +-----------------------+ ");
            attroff(COLOR_PAIR(6));
        } else if (current_mode == Mode::ABOUT) {
            attron(COLOR_PAIR(6));
            mvprintw(my/2-1, mx/2-15, " +--- LIMA FILE MANAGER ---+ ");
            mvprintw(my/2,   mx/2-15, " | Version 1.0 (Pro)       | ");
            mvprintw(my/2+1, mx/2-15, " | Press [ESC] to close    | ");
            mvprintw(my/2+2, mx/2-15, " +-------------------------+ ");
            attroff(COLOR_PAIR(6));
        }

        // 5. Footer
        attron(COLOR_PAIR(2));
        mvhline(my - 1, 0, ' ', mx);
        mvprintw(my - 1, 1, "1Help 3Edit 5Copy 6Move 7Mkdir 8Delete 9Shell 10Quit");
        attroff(COLOR_PAIR(2));

        // THE FIX: Explicitly move cursor to input position as the LAST step
        if (current_mode == Mode::EDITOR) {
            move(cur_y - scroll_y + 2, cur_x + 7);
        } else if (current_mode == Mode::DIALOG) {
            move(my/2, mx/2-20 + 3 + dialog_input.length());
        }
    }

    void run_shell() {
        def_shell_mode(); endwin();
        system("reset"); 
        std::cout << "\033[1;32m--- LIMA PERSISTENT SHELL ---\033[0m\n";
        system("stty echo cooked");
        chdir(current_path.c_str());
        system("/bin/bash");
        reset_shell_mode(); refresh(); load_dir();
        current_mode = Mode::BROWSER;
    }

    void handle_dialog_confirm() {
        try {
            fs::path src = current_path / entries[browser_sel].first;
            fs::path dst = current_path / dialog_input;
            if (dialog_action == 0) fs::copy(src, dst, fs::copy_options::recursive);
            else if (dialog_action == 1) fs::rename(src, dst);
            else if (dialog_action == 2) fs::create_directory(dst);
            else if (dialog_action == 3) { std::ofstream f(dst); }
            else if (dialog_action == 4) {
                std::string cmd = "sudo mount " + dialog_input;
                system(cmd.c_str());
            }
        } catch(...) {}
        load_dir(); current_mode = Mode::BROWSER;
    }

public:
    Lima() {
        setlocale(LC_ALL, ""); initscr(); set_escdelay(25); noecho(); 
        keypad(stdscr, TRUE); timeout(500); init_colors();
        current_path = fs::current_path(); load_dir();
    }
    ~Lima() { endwin(); }

    void handle_input(wint_t ch) {
        if (ch == 27) { // Instant ESC
            nodelay(stdscr, TRUE); wint_t next;
            if (get_wch(&next) == ERR) { current_mode = Mode::BROWSER; curs_set(0); }
            else {
                if (next == 'f' || next == 'F') { current_mode = Mode::MENU; active_menu = 0; menu_sel = 0; }
                if (next == 'c' || next == 'C') { current_mode = Mode::MENU; active_menu = 1; menu_sel = 0; }
                if (next == 'o' || next == 'O') { current_mode = Mode::MENU; active_menu = 2; menu_sel = 0; }
            }
            nodelay(stdscr, FALSE); return;
        }

        if (ch == KEY_F(10)) running = false;
        if (ch == KEY_F(9)) run_shell();
        if (ch == KEY_F(8) && current_mode == Mode::BROWSER) {
            if (entries[browser_sel].first != "..") {
                fs::remove_all(current_path / entries[browser_sel].first); load_dir();
            }
        }
        if (ch == KEY_F(7)) { current_mode = Mode::DIALOG; dialog_title = "New Folder Name:"; dialog_action = 2; dialog_input = ""; }
        if (ch == KEY_F(5)) { current_mode = Mode::DIALOG; dialog_title = "Copy to:"; dialog_action = 0; dialog_input = entries[browser_sel].first + "_cp"; }
        if (ch == KEY_F(6)) { current_mode = Mode::DIALOG; dialog_title = "Move/Rename to:"; dialog_action = 1; dialog_input = entries[browser_sel].first; }

        if (current_mode == Mode::DIALOG) {
            if (ch == '\n') handle_dialog_confirm();
            else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) { if(!dialog_input.empty()) dialog_input.pop_back(); }
            else if (ch >= 32) dialog_input += (char)ch;
        } else if (current_mode == Mode::MENU) {
            if (ch == KEY_UP) menu_sel = std::max(0, menu_sel - 1);
            if (ch == KEY_DOWN) menu_sel = std::min((int)menus[active_menu].size()-1, menu_sel + 1);
            if (ch == '\n') {
                int id = menus[active_menu][menu_sel].id;
                if (id == 0) { current_mode = Mode::DIALOG; dialog_title = "New Folder Name:"; dialog_action = 2; dialog_input = ""; }
                else if (id == 1) { current_mode = Mode::DIALOG; dialog_title = "New File Name:"; dialog_action = 3; dialog_input = ""; }
                else if (id == 2) current_mode = Mode::PROPERTIES;
                else if (id == 3) { if(entries[browser_sel].first != "..") { fs::remove_all(current_path / entries[browser_sel].first); load_dir(); current_mode = Mode::BROWSER; } }
                else if (id == 4) run_shell();
                else if (id == 5) { current_mode = Mode::DIALOG; dialog_title = "Mount (e.g. /dev/sdb1 /mnt):"; dialog_action = 4; dialog_input = ""; }
                else if (id == 6) current_mode = Mode::ABOUT;
                else if (id == 7) running = false;
            }
        } else if (current_mode == Mode::BROWSER) {
            if (ch == KEY_UP && browser_sel > 0) { if(--browser_sel < browser_scroll) browser_scroll--; }
            if (ch == KEY_DOWN && browser_sel < (int)entries.size()-1) { if(++browser_sel >= browser_scroll + (getmaxy(stdscr)-4)) browser_scroll++; }
            if (ch == '\n') {
                auto& e = entries[browser_sel];
                if (e.second) { current_path = (e.first == "..") ? current_path.parent_path() : current_path / e.first; load_dir(); browser_sel = 0; }
                else {
                    buffer.clear(); target_path = current_path / e.first;
                    std::wifstream f(target_path); f.imbue(std::locale(""));
                    std::wstring l; while(std::getline(f,l)) buffer.push_back(l);
                    if(buffer.empty()) buffer.push_back(L"");
                    current_mode = Mode::EDITOR; cur_y = 0; cur_x = 0;
                }
            }
        } else if (current_mode == Mode::EDITOR) {
            if (ch == KEY_F(3)) { // Save & Exit
                std::wofstream f(target_path); f.imbue(std::locale(""));
                for(auto& line : buffer) f << line << L"\n";
                current_mode = Mode::BROWSER;
            }
            else if (ch == KEY_UP && cur_y > 0) { cur_y--; cur_x = std::min(cur_x, (int)buffer[cur_y].size()); }
            else if (ch == KEY_DOWN && cur_y < (int)buffer.size()-1) { cur_y++; cur_x = std::min(cur_x, (int)buffer[cur_y].size()); }
            else if (ch == KEY_LEFT && cur_x > 0) cur_x--;
            else if (ch == KEY_RIGHT && cur_x < (int)buffer[cur_y].size()) cur_x++;
            else if (ch == '\n') {
                buffer.insert(buffer.begin() + cur_y + 1, buffer[cur_y].substr(cur_x));
                buffer[cur_y] = buffer[cur_y].substr(0, cur_x);
                cur_y++; cur_x = 0;
            }
            else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
                if (cur_x > 0) buffer[cur_y].erase(--cur_x, 1);
            }
            else if (ch >= 32) buffer[cur_y].insert(cur_x++, 1, (wchar_t)ch);
            
            if (cur_y < scroll_y) scroll_y = cur_y;
            if (cur_y >= scroll_y + (getmaxy(stdscr)-3)) scroll_y = cur_y - (getmaxy(stdscr)-3) + 1;
        }
    }

    void run() {
        wint_t ch;
        while (running) {
            erase(); draw_ui(); refresh();
            if (get_wch(&ch) != ERR) handle_input(ch);
        }
    }
};

int main() {
    Lima app; app.run(); return 0;
}
