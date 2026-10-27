"""Builds NS_FNB_HullSpray, the spray sheets' Niagara system (/FluidNinjaBuoyancy/Effects, the plugin's Content/Effects).

A fast hull's whisker sheets off its chines and its rooster tail: each sheet is one unattached Niagara component the body moves to
its launch point every tick and drives through five user parameters:

    User.SprayRate      drops per second (0: no new drops; those in the air fall and die where they land)
    User.SprayVelocity  the sheet's launch velocity, world cm/s (the drops leave along it, spread by the cone)
    User.SprayCone      the cone's half angle around the launch velocity, degrees
    User.DropletSize    the drops' size, cm (each drop between 0.6x and 1.4x)
    User.WaterZ         the water's height under the sheet, world cm: a falling drop dies there

Run it from a project that has the plugin built and enabled, in the editor (Tools -> Execute Python Script, or the Python console)
or headless:

    UnrealEditor-Cmd.exe <Project.uproject> -run=pythonscript -script="<path to this file>"

It builds the system only when it is missing; an existing NS_FNB_HullSpray is left as it is once it is checked for the five user
parameters (delete it first to rebuild it from the template). A run that fails after creating the system deletes it again, unsaved,
so a rerun in the same editor session starts afresh. It ends with "HULL SPRAY OK" or raises.
"""
import re

import unreal

ASSET = 'NS_FNB_HullSpray'
FOLDER = '/FluidNinjaBuoyancy/Effects'
TEMPLATE = '/Niagara/DefaultAssets/Templates/Systems/DirectionalBurst'
SPAWN_RATE = '/Niagara/Modules/Emitter/SpawnRate.SpawnRate'
E = 'DirectionalBurst'

try:
    DEV = unreal.FNBDevLibrary
except AttributeError:
    raise RuntimeError('unreal.FNBDevLibrary is missing: run this from a project with the FluidNinja Buoyancy plugin built and enabled')

USER = [
    # name, type, default, description
    ('SprayRate', 'float', '0', 'Drops per second the sheet throws (FluidNinja Buoyancy sets it every tick from the hull\'s speed, the '
     'Spray Sheet Rate and fnb.Effects.SprayRateScale). 0: no new drops; those in the air fall and die where they land. Range 0-2000.'),
    ('SprayVelocity', 'vec3', '0,0,500', 'The sheet\'s launch velocity, world cm/s: 2 V sin(deadrise) out of the chine at about 28 '
     'degrees up, or the rooster tail\'s throw. The drops leave along it, spread by Spray Cone. Its length is their speed.'),
    ('SprayCone', 'float', '18', 'Half angle of the cone the drops leave in around Spray Velocity, degrees. Wider is a looser, more '
     'broken sheet. Default 18 (chines) or 14 (rooster tail); range 0-90.'),
    ('DropletSize', 'float', '4', 'The drops\' sprite size, cm (each drop is between 0.6x and 1.4x it). Default 4 (chines) or 5 '
     '(rooster tail); range 0.5-50.'),
    ('WaterZ', 'float', '0', 'The water\'s height under the sheet, world cm: a drop falling below it dies, where FluidNinja Buoyancy '
     'whitens the water (Spray Landing Foam). Set every tick.'),
]

# (script, module, input, HLSL). User parameters are read by name inside the expressions.
INPUTS = [
    ('ParticleSpawnScript', 'InitializeParticle', 'Lifetime Min', '1.2'),
    ('ParticleSpawnScript', 'InitializeParticle', 'Lifetime Max', '2.4'),
    ('ParticleSpawnScript', 'InitializeParticle', 'Color', 'float4(0.86, 0.93, 1.0, 0.8)'),
    ('ParticleSpawnScript', 'InitializeParticle', 'Uniform Sprite Size Min', 'User.DropletSize * 0.6'),
    ('ParticleSpawnScript', 'InitializeParticle', 'Uniform Sprite Size Max', 'User.DropletSize * 1.4'),
    ('ParticleSpawnScript', 'AddVelocityInCone', 'Velocity Strength', 'length(User.SprayVelocity)'),
    ('ParticleSpawnScript', 'AddVelocityInCone', 'Cone Axis', 'User.SprayVelocity / max(length(User.SprayVelocity), 1.0)'),
    ('ParticleSpawnScript', 'AddVelocityInCone', 'Cone Angle', 'User.SprayCone'),
    # The drops' linear drag (1/s). SprayDropDragPerSecond in FluidNinjaBuoyancyComponent.cpp mirrors it to predict where the drops land
    # (Spray Landing Foam): change both together.
    ('ParticleUpdateScript', 'Drag', 'Drag', '0.35'),
]


def check(report, what):
    if report:
        raise RuntimeError('%s: %s' % (what, report))


def check_existing(path):
    """The NS_FNB_HullSpray already there must take the five user parameters FluidNinja Buoyancy drives (a half-built one does not)."""
    system = unreal.load_asset(path)
    if not isinstance(system, unreal.NiagaraSystem):
        raise RuntimeError('%s exists but does not load as a Niagara system: delete it and run this again' % path)
    summary = DEV.describe_niagara(system, '', '')
    missing = [name for name, _, _, _ in USER if not re.search(r'^\s*User\.%s\s*:' % re.escape(name), summary, re.M)]
    if missing:
        raise RuntimeError('%s exists but lacks the user parameter(s) %s that FluidNinja Buoyancy drives (a half-built system?): delete '
                           'it and run this again to rebuild it from the template' % (path, ', '.join('User.' + name for name in missing)))
    print('HULL SPRAY exists, left as it is (its five user parameters are there):', path)
    return system


def fill(system, path):
    # One emitter: the template's ribbon and its location events go.
    check(DEV.remove_niagara_emitter(system, 'LocationBasedRibbon'), 'remove ribbon')
    check(DEV.set_niagara_module_enabled(system, E, 'ParticleUpdateScript', 'GenerateLocationEvent', False), 'location event')

    for name, kind, default, description in USER:
        exposed = DEV.add_user_variable(system, name, kind, default)
        if exposed.startswith('ERROR'):
            raise RuntimeError('user %s: %s' % (name, exposed))
        check(DEV.set_user_variable_description(system, name, description), 'describe ' + name)

    # A steady stream, not a burst: the emitter runs on its own, looping forever; its spawn rate is SprayRate, interpolated so a sheet
    # moving with a fast hull stays continuous between frames. World space: the drops stay where they were thrown.
    check(DEV.set_niagara_input_enum(system, E, 'EmitterUpdateScript', 'EmitterState', 'Life Cycle Mode', 'Self'), 'life cycle')
    check(DEV.set_niagara_input_enum(system, E, 'EmitterUpdateScript', 'EmitterState', 'Loop Behavior', 'Infinite'), 'loop')
    check(DEV.set_niagara_input_enum(system, '', 'SystemUpdateScript', 'SystemState', 'Loop Behavior', 'Infinite'), 'system loop')
    check(DEV.set_niagara_module_enabled(system, E, 'EmitterUpdateScript', 'SpawnBurst_Instantaneous', False), 'burst off')
    rate = DEV.add_niagara_module_after(system, E, 'EmitterUpdateScript', 'EmitterState', SPAWN_RATE)
    if rate.startswith('ERROR'):
        raise RuntimeError('spawn rate: ' + rate)
    check(DEV.set_niagara_input_linked(system, E, 'EmitterUpdateScript', rate, 'SpawnRate', 'User.SprayRate'), 'rate link')
    check(DEV.set_niagara_emitter_properties(system, E, '{"bLocalSpace": false, "bInterpolatedSpawning": true, "SimTarget": "CPUSim"}'),
          'emitter')

    check(DEV.set_niagara_input_enum(system, E, 'ParticleSpawnScript', 'InitializeParticle', 'Sprite Size Mode', 'Random Uniform'), 'size mode')
    for script, module, name, hlsl in INPUTS:
        check(DEV.set_niagara_input_hlsl(system, E, script, module, name, hlsl), '%s.%s' % (module, name))

    # A falling drop dies at the water (where the body whitens it); a rising one is still leaving the surface.
    alive = DEV.add_niagara_set_parameter_after(system, E, 'ParticleUpdateScript', 'SolveForcesAndVelocity', 'DataInstance.Alive', 'bool')
    if alive.startswith('ERROR'):
        raise RuntimeError('kill at water: ' + alive)
    check(DEV.set_niagara_input_hlsl(system, E, 'ParticleUpdateScript', alive, 'DataInstance.Alive',
                                     'DataInstance.Alive && !(Particles.Position.z < User.WaterZ && Particles.Velocity.z < 0.0)'), 'alive')

    status = DEV.compile_niagara_system(system, True, True)
    print(status)
    if 'errors=1' in status.splitlines()[0]:
        raise RuntimeError('compile failed (above)')
    if not unreal.EditorAssetLibrary.save_loaded_asset(system, False):
        raise RuntimeError('save failed: ' + path)
    print('SAVED:', path)


def build():
    path = '%s/%s' % (FOLDER, ASSET)
    if unreal.EditorAssetLibrary.does_asset_exist(path):
        return check_existing(path)
    template = unreal.load_asset(TEMPLATE)
    if not template:
        raise RuntimeError('no Niagara template at ' + TEMPLATE)
    system, errors = DEV.create_niagara_system(ASSET, FOLDER, template)
    if not system:
        raise RuntimeError('create: ' + errors)
    print('CREATED:', path)
    try:
        fill(system, path)
    except Exception:
        # A half-built system left in memory would pass for the real one on the next run in this session (does_asset_exist sees it):
        # it goes, unsaved, and the error is raised again.
        try:
            deleted = unreal.EditorAssetLibrary.delete_loaded_asset(system)
        except Exception as cleanup:
            deleted = False
            print('delete_loaded_asset failed:', cleanup)
        print(('DELETED (unsaved): %s' if deleted else 'NOT DELETED: delete %s by hand before running this again') % path)
        raise
    return system


build()
print('HULL SPRAY OK')
