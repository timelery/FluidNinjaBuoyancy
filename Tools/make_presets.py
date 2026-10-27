"""Creates or updates FluidNinja Buoyancy's preset library: one FluidNinja Buoyancy Preset data asset per entry of PRESETS, in
/FluidNinjaBuoyancy/Presets (the plugin's Content/Presets). Docs/Presets.md describes them.

Run it from a project that has the plugin built and enabled. Headless:

    UnrealEditor-Cmd.exe <Project.uproject> -run=pythonscript -script="<path to this file>"

or from the editor's Python console (Output Log, set to Python), or Tools -> Execute Python Script. It needs nothing else: no
dialogs, no input, and it opens no window.

It is idempotent. A preset that exists is loaded, one that does not is created, and every property of the class is written on
every preset, so a value not listed in PRESETS is the class's default (DEFAULTS mirrors FluidNinjaBuoyancyPreset.h). Edit a value
below and run it again to update the asset. It prints one line per preset and ends with "PRESETS OK <count>".
"""
import unreal

try:
    PRESET_CLASS = unreal.FluidNinjaBuoyancyPreset
except AttributeError:
    raise RuntimeError('unreal.FluidNinjaBuoyancyPreset is missing: run this from a project with the FluidNinja Buoyancy plugin built and enabled')

FOLDER = '/FluidNinjaBuoyancy/Presets'
EAL = unreal.EditorAssetLibrary
TOOLS = unreal.AssetToolsHelpers.get_asset_tools()
H = unreal.FluidNinjaPresetHull
M = unreal.FluidNinjaPresetMass
A = unreal.FluidNinjaPresetAttitude
I = unreal.FluidNinjaBuoyancyInteraction
F = unreal.FluidNinjaHullFlow
P = unreal.FluidNinjaPropulsionModel

# The class's defaults (FluidNinjaBuoyancyPreset.h), in the order of the header. Every preset writes all of them.
DEFAULTS = {
    'hull': H.FROM_COLLISION,
    'hull_resolution_cm': 25.0,
    'voxel_count': 64,
    'sphere_count': 8,
    'sphere_radius_scale': 1.0,
    'mass': M.DENSITY,
    'density_kg_m3': 500.0,
    'rest_draft_cm': 10.0,
    'attitude': A.AS_PLACED,
    'centre_of_mass_shift': 0.0,
    'settle_on_start': True,
    'sleep_when_settled': True,
    'pressure_drag_linear': 0.3,
    'pressure_drag_quadratic': 1.0,
    'suction_drag_linear': 0.3,
    'suction_drag_quadratic': 0.5,
    'air_drag_coefficient': 0.8,
    'set_interaction': False,
    'interaction': I.WAKES,
    'wake': True,
    'wet_line': True,
    'splash_min_watts': 20.0,
    'max_splash_strength': 12.0,
    'splash_scale': 1.0,
    'spray_sheets': False,
    'spray_deadrise_deg': 18.0,
    'hull_flow': F.DISPLACEMENT,
    'planing_pressure_coefficient': 0.6,
    'set_propulsion': False,
    'propulsion_model': P.PROPELLER,
    'engine_power_kw': 60.0,
    'engine_max_rpm': 5800.0,
    'gear_ratio': 2.0,
    'prop_diameter_cm': 28.0,
    'prop_pitch_ratio': 1.3,
    'steerable_thrust': True,
    'prop_location_share': unreal.Vector(-1.05, 0.0, -1.6),
    'rudder_area_cm2': 500.0,
    'max_rudder_angle': 30.0,
    'keel_area_cm2': 0.0,
    'keel_location_share': unreal.Vector(-0.35, 0.0, -1.0),
    'keel_depth_cm': 15.0,
}

# The library. The key is the asset's name; 'description' is the preset's Description, the rest overrides DEFAULTS.
PRESETS = {
    # 8 kg/m3 is 0.8 kg at 60 cm across: a hollow ball. It never sleeps, so it answers every wave, and it leaves no wake of its
    # own, so it rides the water others push rather than its own.
    'BeachBall': dict(
        description='A light hollow ball (0.8 kg at 60 cm) that bobs on the waves and is carried by the current.',
        hull=H.ONE_SPHERE, mass=M.DENSITY, density_kg_m3=8.0, sleep_when_settled=False, wake=False,
        splash_min_watts=20.0, max_splash_strength=12.0),
    # Cast iron: a 14 cm ball weighs 11 kg and sinks.
    'Cannonball': dict(
        description='Solid iron: it sinks, with a splash the size of its drop.',
        hull=H.ONE_SPHERE, mass=M.DENSITY, density_kg_m3=7800.0, splash_min_watts=20.0),
    # The same stuff as the cannonball, kept as its own preset so a crane's ball is found by name.
    'WreckingBall': dict(
        description='Solid iron, like the cannonball, for a ball swung or dropped from a crane: it sinks.',
        hull=H.ONE_SPHERE, mass=M.DENSITY, density_kg_m3=7800.0, splash_min_watts=20.0),
    # 180 kg/m3: a 1 m wooden crate weighs about 175 kg and floats under a fifth down, where a cube still floats flat.
    'Crate': dict(
        description='A wooden crate (about 175 kg at a metre): it floats high and flat, makes wakes and rides the current.',
        hull=H.VOXELS, voxel_count=64, mass=M.DENSITY, density_kg_m3=180.0, wake=True, splash_min_watts=20.0),
    # Pine, 500 kg/m3: half under, lying flat whichever way the mesh was modelled.
    'Log': dict(
        description='A pine log: it floats half under and lies flat on the water, whichever way its mesh is modelled.',
        hull=H.FROM_COLLISION, hull_resolution_cm=20.0, mass=M.DENSITY, density_kg_m3=500.0, attitude=A.LONGEST_AXIS_FLAT,
        splash_min_watts=20.0),
    # The weight ladder: the same block in four materials. 216 voxels is 6 x 6 x 6 on a cube.
    'PineBlock': dict(
        description='A solid pine block (500 kg/m3): it floats half under.',
        hull=H.VOXELS, voxel_count=216, mass=M.DENSITY, density_kg_m3=500.0, splash_min_watts=20.0),
    'CorkBlock': dict(
        description='A solid cork block (240 kg/m3): it floats high, a quarter under.',
        hull=H.VOXELS, voxel_count=216, mass=M.DENSITY, density_kg_m3=240.0, splash_min_watts=20.0),
    'IceBlock': dict(
        description='A solid block of ice (917 kg/m3): it floats with a tenth above the water.',
        hull=H.VOXELS, voxel_count=216, mass=M.DENSITY, density_kg_m3=917.0, splash_min_watts=20.0),
    'SteelBlock': dict(
        description='A solid steel block (7850 kg/m3): it sinks.',
        hull=H.VOXELS, voxel_count=216, mass=M.DENSITY, density_kg_m3=7850.0, splash_min_watts=20.0),
    # A 30 g arrow of 38 cm3 (790 kg/m3): nine spheres along the shaft, the centre of mass a third of the way toward the head so
    # it floats nose down, and a streamlined shaft's drag rather than nine spheres' (those would take a third of its speed in the
    # first 15 m of flight and stop it 20 cm under). It is shot, not set down, and never sleeps.
    'Arrow': dict(
        description='A 30 g arrow that floats nose down and flies with a streamlined shaft\'s drag. The mesh\'s longest axis must point +X toward the head.',
        hull=H.SPHERE_ROW, sphere_count=9, sphere_radius_scale=1.0, mass=M.DENSITY, density_kg_m3=790.0, centre_of_mass_shift=0.32,
        settle_on_start=False, sleep_when_settled=False, pressure_drag_linear=0.05, pressure_drag_quadratic=0.08,
        suction_drag_linear=0.05, suction_drag_quadratic=0.05, air_drag_coefficient=0.02, splash_min_watts=2.0),
    # Light bodies with Wake off ride the water others push (a swimmer's bow wave) rather than their own wake.
    'RingBuoy': dict(
        description='A ring buoy: eight spheres around the ring, floating 4.5 cm deep and carried by the current.',
        hull=H.SPHERE_RING, sphere_count=8, mass=M.REST_DRAFT, rest_draft_cm=4.5, wake=False, splash_min_watts=20.0),
    'AirMattress': dict(
        description='A large, light, flat air mattress floating 1.2 cm deep; stable steps keep it calm.',
        hull=H.FROM_COLLISION, hull_resolution_cm=20.0, mass=M.REST_DRAFT, rest_draft_cm=1.2, wake=False, splash_min_watts=20.0),
    'Barrel': dict(
        description='A barrel adrift: it lies on its side, floats 25 cm deep and makes wakes.',
        hull=H.FROM_COLLISION, hull_resolution_cm=15.0, mass=M.REST_DRAFT, rest_draft_cm=25.0, attitude=A.LONGEST_AXIS_FLAT, wake=True,
        splash_min_watts=20.0),
    # A boat's splash floor is the project default (300 W): its hull slaps the water all the time.
    'BoatHull': dict(
        description='A small boat\'s hull, 13 cm deep at rest, making wakes. Propulsion (Throttle and Rudder) is left to the component.',
        hull=H.FROM_COLLISION, hull_resolution_cm=30.0, mass=M.REST_DRAFT, rest_draft_cm=13.0, wake=True, splash_min_watts=300.0),
    # A runabout: the planing law, an outboard's engine and propeller (steering with the drive) and spray sheets. The planing hull keeps
    # its finest level (its bottom's facets carry it), so a 25 cm shell. A 2000 cm2 keel a third of the length from the stern holds it in
    # its turns: the V-bottom's own grip against the slide is about a tenth of what a hard turn needs.
    'Motorboat': dict(
        description='A 4-6 m runabout with a 60 kW outboard: it climbs over the hump onto the plane, throws spray sheets and a rooster '
                    'tail, and leaves a white propeller trail. Set Throttle (its sign shifts gear) and Rudder from Blueprint.',
        hull=H.FROM_COLLISION, hull_resolution_cm=25.0, mass=M.REST_DRAFT, rest_draft_cm=14.0, wake=True, splash_min_watts=300.0,
        spray_sheets=True, hull_flow=F.PLANING, set_propulsion=True, keel_area_cm2=2000.0),
}


def summary(values):
    """One phrase for the log line: the hull and the mass rule."""
    hull = values['hull']
    if hull == H.FROM_COLLISION:
        shape = 'from collision at %g cm' % values['hull_resolution_cm']
    elif hull == H.VOXELS:
        shape = '%d voxels' % values['voxel_count']
    elif hull == H.ONE_SPHERE:
        shape = 'one sphere'
    elif hull == H.SPHERE_ROW:
        shape = 'a row of %d spheres' % values['sphere_count']
    else:
        shape = 'a ring of %d spheres' % values['sphere_count']
    mass = values['mass']
    if mass == M.DENSITY:
        weight = '%g kg/m3' % values['density_kg_m3']
    elif mass == M.REST_DRAFT:
        weight = 'rest draft %g cm' % values['rest_draft_cm']
    else:
        weight = 'keeps the body mass'
    return '%s, %s' % (shape, weight)


def make_preset(name, spec):
    """Creates or updates one preset asset and saves it."""
    path = '%s/%s' % (FOLDER, name)
    spec = dict(spec)
    description = spec.pop('description')
    unknown = sorted(set(spec) - set(DEFAULTS))
    if unknown:
        raise KeyError('%s: not preset properties: %s' % (name, ', '.join(unknown)))
    if EAL.does_asset_exist(path):
        asset = EAL.load_asset(path)
        if not isinstance(asset, PRESET_CLASS):
            raise TypeError('%s exists but is a %s, not a FluidNinja Buoyancy Preset' % (path, asset.get_class().get_name()))
        verb = 'MODIFIED'
    else:
        factory = unreal.DataAssetFactory()
        factory.set_editor_property('data_asset_class', PRESET_CLASS)
        asset = TOOLS.create_asset(name, FOLDER, PRESET_CLASS, factory)
        if asset is None:
            raise RuntimeError('could not create ' + path)
        verb = 'CREATED'
    asset.set_editor_property('description', unreal.Text(description))
    values = dict(DEFAULTS, **spec)
    for key, value in values.items():
        asset.set_editor_property(key, value)
    if not EAL.save_asset(path, False):
        raise RuntimeError('could not save ' + path)
    print('%s: %s (%s)' % (verb, path, summary(values)))


def main():
    for name, spec in PRESETS.items():
        make_preset(name, spec)
    print('PRESETS OK %d' % len(PRESETS))


main()
