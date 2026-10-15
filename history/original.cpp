// Original main.cpp — preserved verbatim as a historical relic.
// This is the first-cut simulation the project started from, before any
// modifications (float support, streaming, tie-break fix, growth-from-actives, etc.).
// Do not compile as-is against the current server; this exists purely for reference.

#include <iostream>
#include <cstdlib>
using namespace std;

int generate_best_count(int n, int inactive_odds, int max_odds) {
 int scores[10000];

  for (int count = 1; count < n; count++) { // loop that goes through every member count
    int score = 0;
    int members = count;
    int inactives = 0;

    for (int day = 1; day <= 10000; day++) {

      for (int i = 1; i <= members; i++)
        if (rand() % 100 + 1 <= inactive_odds && inactives <= members-1) inactives++;

      // we should account for information asymmetry somewhat, you only find out a player is inactive after one month of them BEING inactive
      // for now inactivity kinda functions just like them swearing at all the clan members, they are put on death row but dont leave by themselves and you immediately knwo of their crime and they hate eveyrone so they dont do anything.

      while (inactives > 0 && members >= count) {
        inactives--;
        members--;
      }

      float chance = max_odds/(n-1) * members; //the * members assumes it is a linear increase; to be explored later
      int new_members = chance/100; // for chances over 100, 1 member is guaranteed, kinda absurd because this means you cant have odds for a number between 0-2 to be rolled. its only 0-1, 1-2, etc.; to be solved.
      chance -= new_members;

      if (rand() % 100 + 1 <= chance) new_members++;
      if (new_members + members > n) new_members = n - members; // overflow

      // i thought of doing inactives after but it makes more sense that we kick inactives before we get new members to allow most space
      // if (members + new_members > count)

      members += new_members;
      score += members - inactives;
    }

    scores[count] = score;
  }

  int best_score = -1;
  int best_count = 0;
  for (int i = 1; i <= n; i++) {

    if (scores[i] > best_score) {
      best_score = scores[i];
      best_count = i;
    }
  }

  return best_count;
}

int main() {
  int n;
  int inactive_odds;
  int max_odds;

  cout<<"maximum clan members: [enter a number 0-10000]";
  cin>>n;
  cout<<"\n";

  cout<<"odds one clan member becomes inactive in a day [enter a number 0-100]: ";
  cin>>inactive_odds;
  cout<<"\n";

  cout<<"maximum odds of joining (assumed at n-1/n) [enter a number 0-1000]: ";
  cin>>max_odds;
  cout<<"\n";

  cout<<generate_best_count(n, inactive_odds, max_odds);
  return 0;
}
