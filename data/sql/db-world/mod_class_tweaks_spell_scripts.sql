-- mod-class-tweaks: Holy AoE healing tweaks (spell power scaling, Holy Concentration procs)
-- Binds the module's spell script to all ranks of Circle of Healing and Prayer of Healing.
-- The core's own Circle of Healing script stays bound; a spell can have several scripts.
DELETE FROM `spell_script_names` WHERE `ScriptName` = 'spell_class_tweaks_holy_aoe';
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES
(-34861, 'spell_class_tweaks_holy_aoe'),  -- Circle of Healing
(-596,   'spell_class_tweaks_holy_aoe');  -- Prayer of Healing
