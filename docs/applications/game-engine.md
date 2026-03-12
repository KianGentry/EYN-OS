# EYN-OS Generic 2D Game Engine

The EYN-OS game engine has been completely rewritten to support a generic 2D engine that can interpret human-readable text files. This replaces the old binary `.dat` format with a more flexible and user-friendly approach.

## Overview

The new game engine supports:
- Human-readable text file format
- Multiple game types with specific logic (Snake, Maze, Generic)
- Configurable objects and entities
- Level-based games
- Collision detection and combat systems
- Simple AI for enemies
- Health and combat systems
- Collectible items and objectives
- Arrow key and WASD support
- Optional level layouts for certain game types

## Game File Format

Games are defined in `.txt` files with a structured format:

### Global Configuration

```
title: Game Title
description: Game description
controls: Control instructions
board_width: 20
board_height: 15
game_speed: 150
max_entities: 10
max_objects: 50
start_level: 0
```

### Level Definition

Each level is defined with sections:

```
[level]
name: Level Name
width: 20
height: 15

[objects]
wall: type=1, symbol=#, colour=7, solid=1, collectible=0, damage=0, health=0, name=Wall
food: type=3, symbol=*, colour=2, solid=0, collectible=1, damage=0, health=0, name=Food

[entities]
player: x=10, y=7, direction=1, symbol=@, colour=14, health=3, max_health=3, damage=1, speed=1, name=Player, controlled=1, ai_type=0
enemy: x=5, y=5, direction=0, symbol=G, colour=4, health=2, max_health=2, damage=1, speed=2, name=Goblin, controlled=0, ai_type=1

[layout]
00000000000000000000
01111111111111111110
01000000000000000010
01000000000000000010
01111111111111111110
```

## Object Types

- `0` - Empty space
- `1` - Wall (solid)
- `3` - Food (collectible)
- `6` - Door (solid)
- `7` - Key (collectible)
- `8` - Exit (win condition)

## Entity Properties

- `x, y` - Position coordinates
- `direction` - Facing direction (0=up, 1=right, 2=down, 3=left)
- `symbol` - Display character
- `colour` - Display colour (0-15)
- `health` - Current health
- `max_health` - Maximum health
- `damage` - Attack damage
- `speed` - Movement speed
- `controlled` - Player controlled (1) or AI (0)
- `ai_type` - AI behavior type

## Controls

- `WASD` or `Arrow Keys` - Move player
- `P` - Pause/unpause
- `Q` - Quit game

## Game Types

### Snake Game
The engine automatically detects Snake games by title and applies special logic:

- **Continuous Movement**: Snake moves automatically in the current direction
- **Direction-Based Input**: WASD/Arrow keys change direction, snake continues moving
- **Food System**: Food spawns randomly and respawns when eaten
- **Growth Mechanics**: Snake grows when eating food, score increases
- **Collision Detection**: Wall and self-collision cause game over
- **Optional Layout**: If no layout is provided or layout is empty, creates default bordered field
- **Status Display**: Shows "Length: X" instead of health

### Maze Game
Maze games feature exploration and combat:

- **Navigation**: Move through maze to find exit
- **Enemy Combat**: Enemies move randomly and damage player on contact
- **Health System**: Player has health that decreases when hit by enemies
- **Win Condition**: Reaching the exit (E symbol) triggers win
- **Score System**: Defeating enemies increases score

### Generic Games
For other game types, the engine provides:

- **Entity-Based Movement**: Direct position-based movement
- **Object Interaction**: Collectibles, doors, keys, etc.
- **Health Display**: Shows current/max health in status bar

## Example Games

### Snake Game
Located at `testdir/games/snake.txt`

A classic snake game where you control a snake to eat food and grow longer. Features:
- Continuous movement in current direction
- Direction changes with WASD or arrow keys
- Food spawning and snake growth
- Wall and self-collision detection
- Optional custom layouts or default bordered field

### Maze Runner
Located at `testdir/games/maze.txt`

A maze game where you navigate through a labyrinth while avoiding enemies and finding the exit. Features:
- Realistic maze layout with walls
- Enemy AI that moves randomly
- Combat system with health management
- Exit-based win condition
- Score tracking for defeated enemies

## Usage

To run a game:

```
game snake
game maze
```

The engine will automatically add the `.txt` extension if not provided.

## Creating Custom Games

1. Create a `.txt` file in the `testdir/games/` directory
2. Define the game configuration and levels
3. Use the `game` command to run it

### Game Type Detection

The engine automatically detects game types from the title:
- Games with "Snake" in the title use Snake-specific logic
- Games with "Maze" in the title use Maze-specific logic
- Other games use generic entity-based logic

### Level Layout Options

For Snake games, the level layout is optional:
- If omitted or contains only zeros, a default bordered field is created
- If provided, it can be used for custom wall placement
- The snake game ignores generic player entities

## Technical Features

### Memory Management
- Dynamic allocation for game-specific data
- Proper cleanup of all resources
- Support for files larger than 512 bytes

### Input Handling
- Non-blocking input processing
- Support for both WASD and arrow keys
- Game-specific input logic

### Rendering System
- TUI-based display with consistent styling
- Game-specific rendering (snake body, food, etc.)
- Status bar with relevant information

### File System Integration
- Human-readable text file format
- Support for large files (fixed write editor truncation issue)
- Automatic file extension handling

## Recent Improvements

### Write Editor Fix
- Fixed file truncation issue that limited saves to 510 bytes
- Now supports saving files of any size
- Dynamic memory allocation for file content

### Game-Specific Enhancements
- Snake: Continuous movement, optional layouts, proper collision
- Maze: Realistic layouts, working enemies, exit conditions
- Both: Arrow key support, improved status displays

### UI Improvements
- Snake: Top border display, length instead of health
- Maze: Health display, enemy combat feedback
- Both: Single game over message (no duplicate prints)

The engine is designed to be extensible, allowing for new game types and mechanics to be added easily. 