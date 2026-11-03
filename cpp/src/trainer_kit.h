// trainer_kit.h — kit de création de dresseur modulaire (calques PNG)
// Le kit vit dans trainerkit/{walk,run}/<catégorie>/<pièce>/<couleur>.png
// Chaque fichier = feuille 4x4 de 64px (lignes S,W,E,N x 4 pas RMXP).
// La composition produit la colonne 32x1024 du format skins/ (16 marche +
// 16 course, blocs decomp N,S,W,E).
#pragma once
#include "common.h"
#include <string>
#include <vector>

struct KitPiece {
    std::string name;                    // dossier ou fichier (affiché)
    std::vector<std::string> colors;     // variantes (fichier unique -> vide)
};

// catégories dans l'ordre de superposition (bas -> haut)
struct TrainerKit {
    struct Cat {
        std::string dir;                 // nom de dossier dans walk|run
        std::string label[2];            // FR, EN
        bool optional = false;           // "aucun" sélectionnable
        std::vector<KitPiece> pieces;
    };
    std::vector<Cat> cats;               // 6 : peau, bas, haut, cheveux, chapeau, sac
    bool ok = false;
};

struct KitSel { int piece = -1, color = -1; };   // -1 = non choisi / aucun

// retouche couleur d'une catégorie : teinte (°) + sat/val (%) sur tout
// le calque (identity = neutre, rien à faire)
struct KitTint {
    int hue = 0, sat = 100, val = 100;
    bool identity() const { return hue == 0 && sat == 100 && val == 100; }
};

// scanne trainerkit/ : liste pièces + couleurs par catégorie
TrainerKit kit_scan(const std::string& root);

// compose la sélection -> colonne 32x1024 (nullptr vide si incomplet)
// base obligatoire (cat 0) ; les autres calques sont optionnels
// tints[cat] : retouche par calque (nullptr = neutre)
bool kit_compose(const TrainerKit& k, const std::string& root,
                 const KitSel sel[], Image& out,
                 const KitTint tints[] = nullptr);

// compose -> PNG dans <outdir>/<name>.png (skins/ format)
bool kit_save(const TrainerKit& k, const std::string& root,
              const KitSel sel[], const std::string& path,
              const KitTint tints[] = nullptr);

// compose le dos de combat depuis trainerkit/back m|f/ (pose 0, 80x80)
// gender : 0 garçon, 1 fille ; retourne false si le dossier back manque
bool kit_compose_back(const TrainerKit& k, const std::string& root,
                      const KitSel sel[], int gender, Image& out,
                      const KitTint tints[] = nullptr);
