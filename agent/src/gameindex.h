#ifndef GAMEINDEX_H
#define GAMEINDEX_H

/* Ask the game index to check again NOW rather than at its next period - for
 * GAMESYNC, after a run that deployed something, since that is exactly when
 * the host's favourites pipeline wants the index to move. Cheap and safe from
 * any thread; a no-op before gameindex_init(). */
void gameindex_poke(void);

#endif /* GAMEINDEX_H */
